/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>

#define LOG_TAG "BandwidthController"
#include <cutils/log.h>
#include <cutils/properties.h>

extern "C" int logwrap(int argc, const char **argv, int background);

#include "BandwidthController.h"

const int BandwidthController::MAX_CMD_LEN = 1024;
const int BandwidthController::MAX_IFACENAME_LEN = 64;
const int BandwidthController::MAX_CMD_ARGS = 32;
const char BandwidthController::IPTABLES_PATH[] = "/system/bin/iptables";
const char BandwidthController::IP6TABLES_PATH[] = "/system/bin/ip6tables";

/**
 * Some comments about the rules:
 *  * Ordering
 *    - when an interface is marked as costly it should be INSERTED into the INPUT/OUTPUT chains.
 *      E.g. "-I INPUT -i rmnet0 --goto costly"
 *    - quota'd rules in the costly chain should be before penalty_box lookups.
 *
 * * global quota vs per interface quota
 *   - global quota for all costly interfaces uses a single costly chain:
 *    . initial rules
 *      iptables -N costly
 *      iptables -I INPUT -i iface0 --goto costly
 *      iptables -I OUTPUT -o iface0 --goto costly
 *      iptables -I costly -m quota \! --quota 500000 --jump REJECT --reject-with icmp-net-prohibited
 *      iptables -A costly                            --jump penalty_box
 *      iptables -A costly -m owner --socket-exists
 *    . adding a new iface to this, E.g.:
 *      iptables -I INPUT -i iface1 --goto costly
 *      iptables -I OUTPUT -o iface1 --goto costly
 *
 *   - quota per interface. This is achieve by having "costly" chains per quota.
 *     E.g. adding a new costly interface iface0 with its own quota:
 *      iptables -N costly_iface0
 *      iptables -I INPUT -i iface0 --goto costly_iface0
 *      iptables -I OUTPUT -o iface0 --goto costly_iface0
 *      iptables -A costly_iface0 -m quota \! --quota 500000 --jump REJECT --reject-with icmp-net-prohibited
 *      iptables -A costly_iface0                            --jump penalty_box
 *      iptables -A costly_iface0 -m owner --socket-exists
 *
 * * penalty_box handling:
 *  - only one penalty_box for all interfaces
 *   E.g  Adding an app:
 *    iptables -A penalty_box -m owner --uid-owner app_3 --jump REJECT --reject-with icmp-net-prohibited
 */
const char *BandwidthController::cleanupCommands[] = {
/* Cleanup rules. */
"-F", "-t raw -F", "-X costly", "-X penalty_box", };

const char *BandwidthController::setupCommands[] = {
/* Created needed chains. */
"-N costly", "-N penalty_box", };

const char *BandwidthController::basicAccountingCommands[] = { "-F INPUT",
        "-A INPUT -i lo --jump ACCEPT", "-A INPUT -m owner --socket-exists", /* This is a tracking rule. */

        "-F OUTPUT", "-A OUTPUT -o lo --jump ACCEPT", "-A OUTPUT -m owner --socket-exists", /* This is a tracking rule. */

        "-F costly", "-A costly --jump penalty_box", "-A costly -m owner --socket-exists", /* This is a tracking rule. */
        /* TODO(jpa): Figure out why iptables doesn't correctly return from this
         * chain. For now, hack the chain exit with an ACCEPT.
         */
        "-A costly --jump ACCEPT", };

BandwidthController::BandwidthController(void) {

    char value[PROPERTY_VALUE_MAX];

    property_get("persist.bandwidth.enable", value, "0");
    if (!strcmp(value, "1")) {
        enableBandwidthControl();
    }

}

int BandwidthController::runIptablesCmd(const char *cmd, bool isIp6) {
    char buffer[MAX_CMD_LEN];
    const char *argv[MAX_CMD_ARGS];
    int argc = 1;
    char *next = buffer;
    char *tmp;

    argv[0] = isIp6 ? IP6TABLES_PATH : IPTABLES_PATH;
    LOGD("About to run: %s %s", argv[0], cmd);

    strncpy(buffer, cmd, sizeof(buffer) - 1);

    while ((tmp = strsep(&next, " "))) {
        argv[argc++] = tmp;
        if (argc == MAX_CMD_ARGS) {
            LOGE("iptables argument overflow");
            errno = E2BIG;
            return -1;
        }
    }

    argv[argc] = NULL;
    /* TODO(jpa): Once this stabilizes, remove logwrap() as it tends to wedge netd
     * Then just talk directly to the kernel via rtnetlink.
     */
    return logwrap(argc, argv, 0);
}

int BandwidthController::enableBandwidthControl(void) {
    int res;
    /* Some of the initialCommands are allowed to fail */
    runCommands(sizeof(cleanupCommands) / sizeof(char*), cleanupCommands, true, false);
    runCommands(sizeof(cleanupCommands) / sizeof(char*), cleanupCommands, true, true);
    runCommands(sizeof(setupCommands) / sizeof(char*), setupCommands, true, false);
    runCommands(sizeof(setupCommands) / sizeof(char*), setupCommands, true, true);
    res = runCommands(sizeof(basicAccountingCommands) / sizeof(char*), basicAccountingCommands,
                      false, false);
    res |= runCommands(sizeof(basicAccountingCommands) / sizeof(char*), basicAccountingCommands,
                       false, true);
    return res;

}

int BandwidthController::disableBandwidthControl(void) {
    /* The cleanupCommands are allowed to fail. */
    runCommands(sizeof(cleanupCommands) / sizeof(char*), cleanupCommands, true);
    runCommands(sizeof(cleanupCommands) / sizeof(char*), cleanupCommands, true, true);
    return 0;
}

int BandwidthController::runCommands(int numCommands, const char *commands[], bool allowFailure,
                                     bool isIp6) {
    int res = 0;
    LOGD("runCommands(): %d commands", numCommands);
    for (int cmdNum = 0; cmdNum < numCommands; cmdNum++) {
        res = runIptablesCmd(commands[cmdNum], isIp6);
        if (res && !allowFailure)
            return res;
    }
    return allowFailure ? res : 0;
}

std::string BandwidthController::makeIptablesNaughtyCmd(IptOp op, int uid, bool isIp6) {
    std::string res;
    char convBuff[21]; // log10(2^64) ~ 20

    switch (op) {
        case IptOpInsert:
            res = "-I";
            break;
        case IptOpReplace:
            res = "-R";
            break;
        default:
        case IptOpDelete:
            res = "-D";
            break;
    }
    res += " penalty_box";
    sprintf(convBuff, "%d", uid);
    res += " -m owner --uid-owner ";
    res += convBuff;
    res += " --jump REJECT --reject-with";
    if (isIp6) {
        res += " icmp6-adm-prohibited";
    } else {
        res += " icmp-net-prohibited";
    }
    return res;
}

int BandwidthController::addNaughtyApps(int numUids, char *appUids[]) {
    return maninpulateNaughtyApps(numUids, appUids, true);
}

int BandwidthController::removeNaughtyApps(int numUids, char *appUids[]) {
    return maninpulateNaughtyApps(numUids, appUids, false);
}

int BandwidthController::maninpulateNaughtyApps(int numUids, char *appStrUids[], bool doAdd) {
    char cmd[MAX_CMD_LEN];
    int uidNum;
    const char *addFailedTemplate = "Failed to add app uid %d to penalty box.";
    const char *deleteFailedTemplate = "Failed to delete app uid %d from penalty box.";
    IptOp op = doAdd ? IptOpInsert : IptOpDelete;

    int appUids[numUids];
    for (uidNum = 0; uidNum < numUids; uidNum++) {
        appUids[uidNum] = atol(appStrUids[uidNum]);
        if (appUids[uidNum] == 0) {
            LOGE((doAdd ? addFailedTemplate : deleteFailedTemplate), appUids[uidNum]);
            goto fail_parse;
        }
    }

    for (uidNum = 0; uidNum < numUids; uidNum++) {
        std::string naughtyCmd = makeIptablesNaughtyCmd(op, appUids[uidNum], false);
        if (runIptablesCmd(naughtyCmd.c_str())) {
            LOGE((doAdd ? addFailedTemplate : deleteFailedTemplate), appUids[uidNum]);
            goto fail_with_uidNum;
        }
        naughtyCmd = makeIptablesNaughtyCmd(op, appUids[uidNum], true);
        if (runIptablesCmd(naughtyCmd.c_str(), true)) {
            LOGE((doAdd ? addFailedTemplate : deleteFailedTemplate), appUids[uidNum]);
            goto fail_with_uidNum;
        }
    }
    return 0;

    fail_with_uidNum:
    /* Try to remove the uid that failed in any case*/
    runIptablesCmd(makeIptablesNaughtyCmd(IptOpDelete, appUids[uidNum], false).c_str());
    runIptablesCmd(makeIptablesNaughtyCmd(IptOpDelete, appUids[uidNum], true).c_str(), true);
    fail_parse: return -1;
}

std::string BandwidthController::makeIptablesQuotaCmd(IptOp op, char *costName, int64_t quota, bool isIp6) {
    std::string res;
    char convBuff[21]; // log10(2^64) ~ 20
    LOGD("makeIptablesQuotaCmd(%d, %llu, %d)", op, quota, isIp6);
    switch (op) {
        case IptOpInsert:
            res = "-I";
            break;
        case IptOpReplace:
            res = "-R";
            break;
        default:
        case IptOpDelete:
            res = "-D";
            break;
    }
    res += " costly";
    if (costName) {
            res += costName;
    }
    sprintf(convBuff, "%lld", quota);
    res += " -m quota ! --quota ";
    res += convBuff;
    ;
    res += " --jump REJECT --reject-with";
    if (isIp6) {
        res += " icmp6-adm-prohibited";
    } else {
        res += " icmp-net-prohibited";
    }
    return res;
}

int BandwidthController::setInterfaceSharedQuota(int64_t maxBytes, const char *iface) {
    char cmd[MAX_CMD_LEN];
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;

    memset(ifn, 0, sizeof(ifn));
    strncpy(ifn, iface, sizeof(ifn) - 1);

    if (maxBytes == -1) {
        return removeInterfaceSharedQuota(ifn);
    }

    char *costName = NULL;  /* Shared quota */

    /* Insert ingress quota. */
    std::string ifaceName(ifn);
    std::list<QuotaInfo>::iterator it;
    for (it = ifaceRules.begin(); it != ifaceRules.end(); it++) {
        if (it->first == ifaceName)
            break;
    }

    if (it == ifaceRules.end()) {
        snprintf(cmd, sizeof(cmd), "-I INPUT -i %s --goto costly", ifn);
        res |= runIptablesCmd(cmd);
        res |= runIptablesCmd(cmd, true);
        snprintf(cmd, sizeof(cmd), "-I OUTPUT -o %s --goto costly", ifn);
        res |= runIptablesCmd(cmd);
        res |= runIptablesCmd(cmd, true);

        if (ifaceRules.empty()) {
            std::string quotaCmd;
            quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes, false);
            res |= runIptablesCmd(quotaCmd.c_str());
            quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes, true);
            res |= runIptablesCmd(quotaCmd.c_str(), true);
            if (res) {
                LOGE("Failed set quota rule.");
                goto fail;
            }
            sharedQuotaBytes = maxBytes;
        }

        ifaceRules.push_front(QuotaInfo(ifaceName, maxBytes));

    }

    if (maxBytes != sharedQuotaBytes) {
        /* Instead of replacing, which requires being aware of the rules in
         * the kernel, we just add a new one, then delete the older one.
         */
        std::string quotaCmd;

        quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes, false);
        res |= runIptablesCmd(quotaCmd.c_str());
        quotaCmd = makeIptablesQuotaCmd(IptOpInsert, costName, maxBytes, true);
        res |= runIptablesCmd(quotaCmd.c_str(), true);

        quotaCmd = makeIptablesQuotaCmd(IptOpDelete, costName, sharedQuotaBytes, false);
        res |= runIptablesCmd(quotaCmd.c_str());
        quotaCmd = makeIptablesQuotaCmd(IptOpDelete, costName, sharedQuotaBytes, true);
        res |= runIptablesCmd(quotaCmd.c_str(), true);

        if (res) {
            LOGE("Failed replace quota rule.");
            goto fail;
        }
        sharedQuotaBytes = maxBytes;
    }
    return 0;

    fail:
    /*
     * TODO(jpa): once we get rid of iptables in favor of rtnetlink, reparse
     * rules in the kernel to see which ones need cleaning up.
     * For now callers needs to choose if they want to "ndc bandwidth enable"
     * which resets everything.
     */
    removeInterfaceSharedQuota(ifn);
    return -1;
}

int BandwidthController::removeInterfaceSharedQuota(const char *iface) {
    char cmd[MAX_CMD_LEN];
    char ifn[MAX_IFACENAME_LEN];
    int res = 0;

    memset(ifn, 0, sizeof(ifn));
    strncpy(ifn, iface, sizeof(ifn) - 1);

    std::string ifaceName(ifn);
    std::list<QuotaInfo>::iterator it;

    for (it = ifaceRules.begin(); it != ifaceRules.end(); it++) {
        if (it->first == ifaceName)
            break;
    }
    if (it == ifaceRules.end()) {
        LOGE("No such iface %s to delete.", ifn);
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "--delete INPUT -i %s --goto costly", ifn);
    res |= runIptablesCmd(cmd);
    res |= runIptablesCmd(cmd, true);
    snprintf(cmd, sizeof(cmd), "--delete OUTPUT -o %s --goto costly", ifn);
    res |= runIptablesCmd(cmd);
    res |= runIptablesCmd(cmd, true);

    ifaceRules.erase(it);
    if (ifaceRules.empty()) {
        std::string quotaCmd;
        quotaCmd = makeIptablesQuotaCmd(IptOpDelete, NULL, sharedQuotaBytes, false);
        res |= runIptablesCmd(quotaCmd.c_str());

        quotaCmd = makeIptablesQuotaCmd(IptOpDelete, NULL, sharedQuotaBytes, true);
        res |= runIptablesCmd(quotaCmd.c_str(), true);
        sharedQuotaBytes = -1;
    }

    return res;
}