//-----------------------------------------------------------------------------
// Copyright (C) 2021 Merlok
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// CIPURSE transport cards data and commands
//-----------------------------------------------------------------------------

#include "cipursecore.h"

#include "commonutil.h"  // ARRAYLEN
#include "comms.h"       // DropField
#include "util_posix.h"  // msleep
#include <string.h>      // memcpy memset

#include "cmdhf14a.h"
#include "emv/emvcore.h"
#include "emv/emvjson.h"
#include "ui.h"
#include "util.h"

// context for secure channel
CipurseContext cipurseContext;

static int CIPURSEExchangeEx(bool ActivateField, bool LeaveFieldON, sAPDU apdu, bool IncludeLe, uint16_t Le, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    uint8_t data[APDU_RES_LEN] = {0};
    uint8_t securedata[APDU_RES_LEN] = {0};
    sAPDU secapdu;

    *ResultLen = 0;
    if (sw) *sw = 0;
    uint16_t isw = 0;
    int res = 0;

    if (ActivateField) {
        DropField();
        msleep(50);
    }
    
    // long messages is not allowed
    if (apdu.Lc > 228)
        return 20;

    // COMPUTE APDU
    int datalen = 0;
    uint16_t xle = IncludeLe ? 0x100 : 0x00;
    if (xle == 0x100 && Le != 0)
        xle = Le;

    CipurseCAPDUReqEncode(&cipurseContext, &apdu, &secapdu, securedata, IncludeLe, Le);
    
    if (APDUEncodeS(&secapdu, false, xle, data, &datalen)) {
        PrintAndLogEx(ERR, "APDU encoding error.");
        return 201;
    }

    if (GetAPDULogging())
        PrintAndLogEx(SUCCESS, ">>>> %s", sprint_hex(data, datalen));

    res = ExchangeAPDU14a(data, datalen, ActivateField, LeaveFieldON, Result, (int)MaxResultLen, (int *)ResultLen);
    if (res) {
        return res;
    }

    if (GetAPDULogging())
        PrintAndLogEx(SUCCESS, "<<<< %s", sprint_hex(Result, *ResultLen));
    
    if (*ResultLen < 2) {
        return 200;
    }
    
    size_t rlen = 0;
    if (*ResultLen == 2) {
        if (cipurseContext.RequestSecurity == CPSMACed || cipurseContext.RequestSecurity == CPSEncrypted)
            CipurseCClearContext(&cipurseContext);

        isw = Result[0] * 0x0100 + Result[1];
    } else {
        CipurseCAPDURespDecode(&cipurseContext, Result, *ResultLen, securedata, &rlen, &isw);
        memcpy(Result, securedata, rlen);
    }
    
    if (ResultLen != NULL)
        *ResultLen = rlen;
    
    if (sw != NULL)
        *sw = isw;

    if (isw != 0x9000) {
        if (GetAPDULogging()) {
            if (*sw >> 8 == 0x61) {
                PrintAndLogEx(ERR, "APDU chaining len:%02x -->", *sw & 0xff);
            } else {
                PrintAndLogEx(ERR, "APDU(%02x%02x) ERROR: [%4X] %s", apdu.CLA, apdu.INS, isw, GetAPDUCodeDescription(*sw >> 8, *sw & 0xff));
                return 5;
            }
        }
    }

    return PM3_SUCCESS;
}

/*static int CIPURSEExchange(sAPDU apdu, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    return CIPURSEExchangeEx(false, true, apdu, true, 0, Result, MaxResultLen, ResultLen, sw);
}*/

int CIPURSESelect(bool ActivateField, bool LeaveFieldON, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    uint8_t data[] = {0x41, 0x44, 0x20, 0x46, 0x31};
    CipurseCClearContext(&cipurseContext);

    return EMVSelect(ECC_CONTACTLESS, ActivateField, LeaveFieldON, data, sizeof(data), Result, MaxResultLen, ResultLen, sw, NULL);
}

int CIPURSEChallenge(uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    return CIPURSEExchangeEx(false, true, (sAPDU) {0x00, 0x84, 0x00, 0x00, 0x00, NULL}, true, 0x16, Result, MaxResultLen, ResultLen, sw);
}

int CIPURSEMutalAuthenticate(uint8_t keyIndex, uint8_t *params, uint8_t paramslen, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    return CIPURSEExchangeEx(false, true, (sAPDU) {0x00, 0x82, 0x00, keyIndex, paramslen, params}, true, 0x10, Result, MaxResultLen, ResultLen, sw);
}

int CIPURSESelectFile(uint16_t fileID, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    uint8_t fileIdBin[] = {fileID >> 8, fileID & 0xff};
    return CIPURSEExchangeEx(false, true, (sAPDU) {0x00, 0xa4, 0x00, 0x00, 02, fileIdBin}, true, 0, Result, MaxResultLen, ResultLen, sw);
}

int CIPURSEReadFileAttributes(uint8_t *data, uint16_t *datalen) {
    //CIPURSEExchangeEx(false, true, (sAPDU) {0x00, 0x82, 0x00, keyIndex, paramslen, params}, true, 0x10, Result, MaxResultLen, ResultLen, sw);
    return 2;
}

int CIPURSEReadBinary(uint16_t offset, uint8_t *Result, size_t MaxResultLen, size_t *ResultLen, uint16_t *sw) {
    return CIPURSEExchangeEx(false, true, (sAPDU) {0x00, 0xb0, (offset >> 8) & 0x7f, offset & 0xff, 0, NULL}, true, 0, Result, MaxResultLen, ResultLen, sw);
}

bool CIPURSEChannelAuthenticate(uint8_t keyIndex, uint8_t *key, bool verbose) {
    uint8_t buf[APDU_RES_LEN] = {0};
    size_t len = 0;
    uint16_t sw = 0;

    CipurseContext cpc = {0};
    CipurseCSetKey(&cpc, keyIndex, key);
    
    // get RP, rP
    int res = CIPURSEChallenge(buf, sizeof(buf), &len, &sw);
    if (res != 0 || len != 0x16) {
        if (verbose)
            PrintAndLogEx(ERR, "Cipurse get challenge " _RED_("error") ". Card returns 0x%04x.", sw);
        
        return false;
    }
    CipurseCSetRandomFromPICC(&cpc, buf);

    // make auth data
    uint8_t authparams[16 + 16 + 6] = {0};
    CipurseCAuthenticateHost(&cpc, authparams);
    
    // authenticate
    res = CIPURSEMutalAuthenticate(keyIndex, authparams, sizeof(authparams), buf, sizeof(buf), &len, &sw);
    if (res != 0 || sw != 0x9000 || len != 16) {
        if (sw == 0x6988) {
            if (verbose)
                PrintAndLogEx(ERR, "Cipurse authentication " _RED_("error") ". Wrong key.");
        } else if (sw == 0x6A88) {
            if (verbose)
                PrintAndLogEx(ERR, "Cipurse authentication " _RED_("error") ". Wrong key number.");
        } else {
            if (verbose)
                PrintAndLogEx(ERR, "Cipurse authentication " _RED_("error") ". Card returns 0x%04x.", sw);
        }
        
        CipurseCClearContext(&cipurseContext);
        return false;
    }
    
    if (CipurseCCheckCT(&cpc, buf)) {
        if (verbose)
            PrintAndLogEx(INFO, "Authentication " _GREEN_("OK"));
        
        CipurseCChannelSetSecurityLevels(&cpc, CPSMACed, CPSMACed);
        memcpy(&cipurseContext, &cpc, sizeof(CipurseContext));
        return true;
    } else {
        if (verbose)
            PrintAndLogEx(ERR, "Authentication " _RED_("ERROR") " card returned wrong CT");
        
        CipurseCClearContext(&cipurseContext);
        return false;
    }
}

void CIPURSECSetActChannelSecurityLevels(CipurseChannelSecurityLevel req, CipurseChannelSecurityLevel resp) {
    CipurseCChannelSetSecurityLevels(&cipurseContext, req, resp);
}

void CIPURSEPrintInfoFile(uint8_t *data, size_t len) {
    if (len < 2) {
        PrintAndLogEx(ERR, "Info file length " _RED_("ERROR"));
        return;
    }

    PrintAndLogEx(INFO, "------------ INFO ------------");
    PrintAndLogEx(INFO, "CIPURSE version %d revision %d", data[0], data[1]);
}

