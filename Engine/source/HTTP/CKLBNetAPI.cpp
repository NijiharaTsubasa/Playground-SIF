﻿/* 
   Copyright 2013 KLab Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "CKLBNetAPI.h"
#include "CKLBLuaEnv.h"
#include "CKLBUtility.h"
#include "CKLBJsonItem.h"
#include "CPFInterface.h"
#include "CKLBNetAPIKeyChain.h"
#include "base64.h"

#include <time.h>
#include <ctype.h>

enum {
	// Command Values定義
	NETAPI_SEND,				// send JSON packet
	NETAPI_CANCEL,				// selected session cancel
	NETAPI_CANCEL_ALL,			// cancel all sessions [not_implemented]
	NETAPI_BUSY,				// current connection is busy
	NETAPI_STARTUP,				// login/authKey -> login/startUp
	NETAPI_LOGIN,				// login/authKey -> login/login
	NETAPI_WATCH_MAINTENANCE,	// check for maintenance? [not_used]
	NETAPI_DEBUG_HDR,			// set "Debug: 1" header [not_used]
	NETAPI_GEN_CMDNUMID,		// generate commandNum string [not_used]
};

static IFactory::DEFCMD cmd[] = {
	{"NETAPI_SEND",					NETAPI_SEND					},
	{"NETAPI_CANCEL",				NETAPI_CANCEL				},
    {"NETAPI_CANCEL_ALL",			NETAPI_CANCEL_ALL			},
	{"NETAPI_BUSY",					NETAPI_BUSY					},
	{"NETAPI_STARTUP",				NETAPI_STARTUP				},
	{"NETAPI_LOGIN",				NETAPI_LOGIN				},
	{"NETAPI_WATCH_MAINTENANCE",	NETAPI_WATCH_MAINTENANCE	},
	{"NETAPI_DEBUG_HDR",			NETAPI_DEBUG_HDR			},
	{"NETAPI_GEN_CMDNUMID",			NETAPI_GEN_CMDNUMID			},

	//
	// Callback constants
	//
	{ "NETAPIMSG_CONNECTION_CANCELED",	NETAPIMSG_CONNECTION_CANCELED },
	{ "NETAPIMSG_CONNECTION_FAILED",	NETAPIMSG_CONNECTION_FAILED },
	{ "NETAPIMSG_INVITE_FAILED",		NETAPIMSG_INVITE_FAILED },
	{ "NETAPIMSG_STARTUP_FAILED",		NETAPIMSG_STARTUP_FAILED },
	{ "NETAPIMSG_LOGIN_FAILED",			NETAPIMSG_LOGIN_FAILED },
	{ "NETAPIMSG_SERVER_TIMEOUT",		NETAPIMSG_SERVER_TIMEOUT },
	{ "NETAPIMSG_REQUEST_FAILED",		NETAPIMSG_REQUEST_FAILED },
	{ "NETAPIMSG_SERVER_ERROR",			NETAPIMSG_SERVER_ERROR },
	{ "NETAPIMSG_UNKNOWN",				NETAPIMSG_UNKNOWN },
	{ "NETAPIMSG_REQUEST_SUCCESS",		NETAPIMSG_REQUEST_SUCCESS },
	{ "NETAPIMSG_LOGIN_SUCCESS",		NETAPIMSG_LOGIN_SUCCESS },
	{ "NETAPIMSG_STARTUP_SUCCESS",		NETAPIMSG_STARTUP_SUCCESS },
	{ "NETAPIMSG_INVITE_SUCCESS",		NETAPIMSG_INVITE_SUCCESS },
	{0, 0}
};

static CKLBTaskFactory<CKLBNetAPI> factory("HTTP_API", CLS_KLBNETAPI, cmd);

CKLBNetAPI::CKLBNetAPI()
: CKLBLuaTask           ()
, m_http				(NULL)
, m_timeout				(30000)
, m_timestart			(0)
, m_canceled			(false)
, m_pRoot				(NULL)
, m_callback			(NULL)
, m_verup_callback		(NULL)
, m_http_header_array	(NULL)
, m_http_header_length	(0)
, m_nonce				(1)
, m_handle				(0)
, m_skipVersionCheck	(false)
{
}

CKLBNetAPI::~CKLBNetAPI() 
{
	// Done in Die()
}

u32 
CKLBNetAPI::getClassID() 
{
	return CLS_KLBNETAPI;
}

void
CKLBNetAPI::execute(u32 deltaT)
{
	if (!m_http) {
		return; // Do nothing if no active connection
	}

	m_timestart += deltaT;

	// Check cancel first
	if (m_canceled) {
		// release connection first
		releaseConnection();
		// and after that call callback
		lua_callback(NETAPIMSG_CONNECTION_CANCELED, -1, NULL, m_nonce);
		m_canceled = false;
		return;
	}

	// Received data second
	if (m_http->httpRECV() || (m_http->getHttpState() != -1)) {
		CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
		// Get Data
		u8* body	= m_http->getRecvResource();
		u32 bodyLen	= body ? m_http->getSize() : 0;
		
		// Get Status Code
		int state = m_http->getHttpState();
		bool invalid = ((state >= 400) && (state <= 599)) || (state == 204);
		int msg = invalid == false ? NETAPIMSG_REQUEST_SUCCESS : mapFail();
		if (m_skipVersionCheck == false && invalid == false && m_lastCommand == NETAPI_SEND) {
			const char* server_ver[2];
			if (m_http->hasHeader("Server-Version", server_ver))
			{
				if (strncmp(*server_ver, kc.getClient(), strlen(kc.getClient())))
				{
					releaseConnection();
					CKLBScriptEnv::getInstance().call_netAPI_versionUp(m_verup_callback, this, kc.getClient(), *server_ver);
					return;
				}
			}
		}

		if (m_http->isMaintenance() || m_http->isOutdated()) {
			// enter into maintenance mode
			CKLBLuaEnv::getInstance().intoMaintenance(m_http->isOutdated());
			releaseConnection();
			return;
		}

		//
		// Support only JSon for callback
		// 
		freeJSonResult();
		if (bodyLen > 0) m_pRoot = getJsonTree((const char*)body, bodyLen);

		// Release connection
		releaseConnection();
		if (m_pRoot == NULL) {
			lua_callback(msg, state, NULL, m_nonce);
			return;
		}

		if (invalid == false) {
			m_nonce++; // increase nonce if request success
			if (m_handle == NETAPIHDL_AUTHKEY_RESPONSE) {
				authKey();
			}
			if (m_lastCommand == NETAPI_STARTUP) {
				return startUp(state);
			}
				
			if (m_lastCommand == NETAPI_LOGIN) {
				return login(state);
			}
				
			lua_callback(msg, state, m_pRoot, m_nonce - 1);
			return;
		}
		lua_callback(msg, state, m_pRoot, m_nonce);
		return;
	}

	if ((m_http->m_threadStop == 1) && (m_http->getHttpState() == -1)) {
		releaseConnection();
		lua_callback(NETAPIMSG_CONNECTION_FAILED, -1, NULL, m_nonce);
		return;
	}

	// Time out third (after check that valid has arrived)
	if (m_timestart >= m_timeout) {
		releaseConnection();
		lua_callback(NETAPIMSG_SERVER_TIMEOUT, -1, NULL, m_nonce);
		return;
	}
}

void
CKLBNetAPI::die()
{
	if (m_http) {
		NetworkManager::releaseConnection(m_http);
	}
	KLBDELETEA(m_callback);
	freeHeader();
	freeJSonResult();
}

void
CKLBNetAPI::freeJSonResult() {
	KLBDELETE(m_pRoot);
}

void
CKLBNetAPI::releaseConnection() {
	NetworkManager::releaseConnection(m_http);
	m_http = NULL;
}

void
CKLBNetAPI::freeHeader() {
	if (m_http_header_array) {
		for (u32 n=0; n < m_http_header_length; n++) {
			KLBDELETEA(m_http_header_array[n]);
		}
		KLBDELETEA(m_http_header_array);
		m_http_header_array = NULL;
	}
}
int
CKLBNetAPI::mapFail() {
	switch (m_lastCommand) {
	case NETAPI_STARTUP:
		return NETAPIMSG_STARTUP_FAILED;
	case NETAPI_LOGIN:
		return NETAPIMSG_LOGIN_FAILED;
	case NETAPI_SEND:
		return NETAPIMSG_REQUEST_FAILED;
	default:
		return NETAPIMSG_SERVER_ERROR;
	}
}

CKLBNetAPI* 
CKLBNetAPI::create( CKLBTask* pParentTask, 
                    const char * callback) 
{
	CKLBNetAPI* pTask = KLBNEW(CKLBNetAPI);
    if(!pTask) { return NULL; }

	if(!pTask->init(pParentTask, callback)) {
		KLBDELETE(pTask);
		return NULL;
	}
	return pTask;
}

bool 
CKLBNetAPI::init(	CKLBTask* pTask,
					const char * callback)
{
	m_callback = (callback) ? CKLBUtility::copyString(callback) : NULL;

	// 一通り初期化値が作れたのでタスクを登録
	bool res = regist(pTask, P_INPUT);
	return res;
}

bool
CKLBNetAPI::initScript(CLuaState& lua)
{
	int argc = lua.numArgs();

    if (argc < 7) { 
		lua.retBoolean(false);
		return false; 
	}
	lua.printStack();
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	kc.setUrl(lua.getString(1));
	kc.setConsumernKey(lua.getString(2));
	kc.setClient(lua.getString(3));
	kc.setAppID(lua.getString(4));
	kc.setRegion(lua.getString(7));
	if (lua.isString(8)) m_verup_callback = CKLBUtility::copyString(lua.getString(8));

	return init(NULL, lua.getString(5));
}

CKLBJsonItem *
CKLBNetAPI::getJsonTree(const char * json_string, u32 dataLen)
{
	CKLBJsonItem * pRoot = CKLBJsonItem::ReadJsonData((const char *)json_string, dataLen);

	return pRoot;
}

void
CKLBNetAPI::authKey()
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	IPlatformRequest& platform = CPFInterface::getInstance().platform();

	char requestData[1024];
	const char* form[2];

	switch (m_handle) {
	case NETAPIHDL_AUTHKEY_REQUEST: {
		m_http = NetworkManager::createConnection();
		m_http->reuse();
		// dummy token part
		const char* clientKey = kc.getClientKey();
		unsigned char clientKeyEncrypted[512];
		int clientKeyEncryptedLen = platform.publicKeyEncrypt((unsigned char*)clientKey, strlen(clientKey), clientKeyEncrypted);
		int dummyTokenLen = 0;
		char* dummyToken = base64(clientKeyEncrypted, clientKeyEncryptedLen, &dummyTokenLen);

		// auth data part
		char devData[512];
		sprintf(devData, "{ \"1\":\"%s\",\"2\": \"%s\", \"3\": \"eyJSYXRpbmciOiIwIiwiRGV0YWlsIiA6ICJUaGlzIGlzIGEgaU9TIGRldmljZSJ9\"}", kc.getLoginKey(), kc.getLoginPwd());
		unsigned char devDataEnc[512];
		int devDataEncLen = platform.encryptAES128CBC(devData, clientKey, clientKey + 16, devDataEnc);
		int authDataLen = 0;
		char* authData = base64(devDataEnc, devDataEncLen, &authDataLen);
		sprintf(requestData, "request_data={\"dummy_token\":\"%s\", \"auth_data\":\"%s\"}", dummyToken, authData);

		form[0] = requestData;
		form[1] = NULL;
		setHeaders(requestData);
		m_http->setForm(form);

		char url[MAX_PATH];
		sprintf(url, "%s/login/authkey", kc.getUrl());
		m_http->httpPOST(url, false);

		free(dummyToken);
		free(authData);
		m_handle = NETAPIHDL_AUTHKEY_RESPONSE;
		break;
	}
	case NETAPIHDL_AUTHKEY_RESPONSE: {
		kc.setToken(m_pRoot->child()->child()->getString());
		const char* dummyToken = m_pRoot->child()->child()->next()->getString();
		klb_assert(dummyToken, "Dummy token does not exist!!!");
		int len = 0;
		unsigned char* unbasedToken = unbase64(dummyToken, strlen(dummyToken), &len);

		const char* clientKey = kc.getClientKey();
		char sessionKey[33] = ""; // + 1 because of null terminator
		for (int i = 0; i < 32; i++) {
			sessionKey[i] = unbasedToken[i] ^ clientKey[i];
		}
		kc.setSessionKey(sessionKey);
		m_handle = m_lastCommand == NETAPI_LOGIN ? NETAPIHDL_LOGIN_REQUEST : NETAPIHDL_STARTUP_REQUEST;
		break;
	}
	}
}

void
CKLBNetAPI::login(int status)
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	IPlatformRequest& platform = CPFInterface::getInstance().platform();

	switch (m_handle) {
	case NETAPIHDL_LOGIN_REQUEST: {
		m_http = NetworkManager::createConnection();
		m_http->reuse();
		char requestData[512];
		const char* form[2];

		const char* loginKey = kc.getLoginKey();
		const char* loginPwd = kc.getLoginPwd();
		const char* sessionKey = kc.getSessionKey();

		// encrypt credentials
		unsigned char loginKeyEnc[128];
		unsigned char loginPwdEnc[256];
		int loginKeyEncLen = platform.encryptAES128CBC(loginKey, sessionKey, sessionKey + 16, loginKeyEnc);
		int loginPwdEncLen = platform.encryptAES128CBC(loginPwd, sessionKey, sessionKey + 16, loginPwdEnc);

		// and do base64
		int loginKeyBLen = 0;
		int loginPwdBLen = 0;
		char* loginKeyB = base64(loginKeyEnc, loginKeyEncLen, &loginKeyBLen);
		char* loginPwdB = base64(loginPwdEnc, loginPwdEncLen, &loginPwdBLen);

		sprintf(requestData, "request_data={\"login_key\": \"%s\",\"login_passwd\": \"%s\"}", loginKeyB, loginPwdB);
		form[0] = requestData;
		form[1] = NULL;
		setHeaders(requestData);
		m_http->setForm(form);

		char URL[MAX_PATH];
		sprintf(URL, "%s/login/login", kc.getUrl());
		m_http->httpPOST(URL, false);
		m_timestart = 0;
		m_handle = NETAPIHDL_LOGIN_RESPONSE;

		free(loginKeyB);
		free(loginPwdB);
		break;
	}
	case NETAPIHDL_LOGIN_RESPONSE: {
		char userID[16];
		sprintf(userID, "%d", m_pRoot->child()->child()->next()->getInt());
		kc.setToken(m_pRoot->child()->child()->getString());
		kc.setUserID(userID);
		m_handle = -1;
		lua_callback(NETAPIMSG_LOGIN_SUCCESS, status, m_pRoot, 1);
	}
	}
}

void
CKLBNetAPI::startUp(int status)
{
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
	IPlatformRequest& platform = CPFInterface::getInstance().platform();

	switch (m_handle) {
	case NETAPIHDL_STARTUP_REQUEST: {
		m_http = NetworkManager::createConnection();
		m_http->reuse();

		char requestData[512];
		const char* form[2];

		const char* loginKey = kc.getLoginKey();
		const char* loginPwd = kc.getLoginPwd();
		const char* sessionKey = kc.getSessionKey();

		// encrypt credentials
		unsigned char loginKeyEnc[128];
		unsigned char loginPwdEnc[256];
		int loginKeyEncLen = platform.encryptAES128CBC(loginKey, sessionKey, sessionKey + 16, loginKeyEnc);
		int loginPwdEncLen = platform.encryptAES128CBC(loginPwd, sessionKey, sessionKey + 16, loginPwdEnc);

		// and do base64
		int loginKeyBLen = 0;
		int loginPwdBLen = 0;
		char* loginKeyB = base64(loginKeyEnc, loginKeyEncLen, &loginKeyBLen);
		char* loginPwdB = base64(loginPwdEnc, loginPwdEncLen, &loginPwdBLen);

		sprintf(requestData, "request_data={\"login_key\": \"%s\",\"login_passwd\": \"%s\"}", loginKeyB, loginPwdB);
		form[0] = requestData;
		form[1] = NULL;
		setHeaders(requestData);
		m_http->setForm(form);

		char URL[MAX_PATH];
		sprintf(URL, "%s/login/startUp", kc.getUrl());
		m_http->httpPOST(URL, false);
		m_timestart = 0;
		m_handle = NETAPIHDL_STARTUP_RESPONSE;

		free(loginKeyB);
		free(loginPwdB);
		break;
	}
	case NETAPIHDL_STARTUP_RESPONSE: {
		kc.setToken(NULL);
		m_nonce = 1;
		m_handle = 0;
		lua_callback(NETAPIMSG_STARTUP_SUCCESS, status, m_pRoot, 0);
	}
	}
	
}

void
CKLBNetAPI::setHeaders(const char* data, const char* key)
{
	CPFInterface& pfif = CPFInterface::getInstance();
	CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();

	// TODO
	const char* headers[13];

	// For values above
	char* alldata = new char[1280];
	char* os_data = alldata;
	char* os_version = alldata + 128;
	char* time_zone = alldata + 256;
	char* application_id = time_zone + 128;
	char* authorize = NULL; // Special
	char* bundle_version = time_zone + 256;
	char* client_version = bundle_version + 128;
	char* region = bundle_version + 256;
	char* xmc = region + 64;
	char* user_id = xmc + 128;

	// Process authorize string
	authorize = new char[1024];

	if (data != NULL) {
		char temp[41];
		char* signKey = (char*)key;
		if (signKey == NULL)
			signKey = (char*)kc.getSessionKey();
		pfif.platform().HMAC_SHA1(data + 13, signKey, temp);
		sprintf(xmc, "X-Message-Code: %s", temp);
	}


	sprintf(authorize, "Authorize: %s", kc.getAuthorizeString(m_nonce));
	sprintf(application_id, "Application-ID: %s", kc.getAppID());
	sprintf(bundle_version, "Bundle-Version: %s", pfif.platform().getBundleVersion());
	sprintf(client_version, "Client-Version: %s", kc.getClient());
	sprintf(region, "Region: %s", kc.getRegion());

	// User-ID
	const char* uid = kc.getUserID();

	if (uid == NULL)
		user_id = NULL;
	else
		sprintf(user_id, "User-ID: %s", uid);

	// Set header
	headers[0] = "API-Model: straightforward";
	headers[1] = application_id;
	headers[2] = authorize;
	headers[3] = bundle_version;
	headers[4] = client_version;
	headers[5] = "Debug: 1";
	headers[6] = "OS: Android";
	headers[7] = "OS-Version: Nexus 5 google hammerhead 4.4.4";
	headers[8] = "Platform-Type: 2";
	headers[9] = region;
	headers[10] = xmc;
	headers[11] = user_id;
	headers[12] = NULL;

	m_http->setHeader(headers);

	delete[] authorize;
	delete[] alldata;
}

int
CKLBNetAPI::commandScript(CLuaState& lua)
{
	int argc = lua.numArgs();

	if(argc < 2) {
		lua.retBoolean(false);
		return 1;
	}

	int cmd = m_lastCommand = lua.getInt(2);
	int ret = 1;

	switch(cmd)
	{
	default:
		{
			lua.retBoolean(false);
		}
		break;
	case NETAPI_STARTUP:
		{
			// 3. login_key
			// 4. login_passwd
			// 5. nil?
			// 6. timeout
			// 7. session key
			// 8. client key
			if (argc < 8) {
				lua.retBool(false);
			}
			const char* login = lua.getString(3);
			const char* pass  = lua.getString(4);
			const char* sKey  = lua.getString(7);
			const char* cKey  = lua.getString(8);

			// save cred
			CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
			kc.setLoginKey(login);
			kc.setLoginPwd(pass);
			kc.setSessionKey(sKey);
			kc.setClientKey(cKey);

			m_timeout = lua.getInt(6);
			m_timestart = 0;
			m_handle = NETAPIHDL_AUTHKEY_REQUEST;
			authKey();
			lua.retBoolean(true);
		}
		break;
	case NETAPI_LOGIN:
		{
			lua.printStack();
			if (argc < 7) {
				lua.retBool(false);
			}
			const char* login = lua.getString(3);
			const char* pass = lua.getString(4);
			const char* sKey = lua.getString(6);
			const char* cKey = lua.getString(7);

			// save cred
			CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
			kc.setLoginKey(login);
			kc.setLoginPwd(pass);
			kc.setSessionKey(sKey);
			kc.setClientKey(cKey);

			m_timeout = lua.getInt(5);
			m_timestart = 0;
			m_handle = NETAPIHDL_AUTHKEY_REQUEST;
			authKey();
			lua.retInt(m_nonce);
		}
		break;
	case NETAPI_WATCH_MAINTENANCE: 
		{

		}
		break;
	case NETAPI_DEBUG_HDR:
		{

		}
		break;
	case NETAPI_GEN_CMDNUMID:
		{

		}
		break;
	case NETAPI_CANCEL_ALL:
	case NETAPI_CANCEL:
		{
			if (m_http != NULL) {
				m_canceled = true;
			}
			lua.retBoolean(m_http != NULL);
		}
		break;
	case NETAPI_BUSY:
		{
			// If object is alive, then it is busy.
			lua.retBoolean(m_http != NULL);
		}
		break;
	case NETAPI_SEND:
		{
			//
			//	3. Request data table
			//	4. End point. "/api" if nil
			//	5. Timeout
			//	7. Skip version check?
			lua.printStack();
			if(argc < 3 || argc > 9) {
				klb_assertAlways("Too more or not enough args");
				lua.retBoolean(false);
			}
			else
			if (m_http) {
				// Connection is still busy,you can not send
				klb_assertAlways("trying to use connection while it busy");
				lua.retBoolean(false);
			}
			else {
				CKLBNetAPIKeyChain& kc = CKLBNetAPIKeyChain::getInstance();
				char URL[MAX_PATH];
				const char* endPoint = "/api";
				if(!lua.isNil(4)) endPoint = lua.getString(4);
				sprintf(URL, "%s%s", kc.getUrl(), endPoint);

				m_skipVersionCheck = lua.getBoolean(7);
				const char* specialKey = NULL;
				if (lua.isString(9)) specialKey = lua.getString(9);

				// JSON payload (method POST)
				freeHeader();
				u32 send_json_size = 0;
				const char* send_json = NULL;

				m_http = NetworkManager::createConnection();

				if (m_http) {
					lua.retValue(3);
					send_json = CKLBUtility::lua2json(lua, send_json_size);
					lua.pop(1);

					if (send_json) {
						char* json;
						const char * items[2];
						const char* req = "request_data=";
						int send_json_length = strlen(send_json);
						int req_length = strlen(req);

						json = KLBNEWA(char, send_json_length + req_length + 1);
						strcpy( json , req );
						strcat( json , send_json );
						items[0] = json;
						items[1] = NULL;

						setHeaders(json, specialKey);
						m_http->setForm(items);
						m_http->httpPOST(URL, false);

						KLBDELETEA(json);
					} else {
						m_http->httpGET(URL, false);
					}

					m_timeout	= lua.getInt(5);
					m_timestart = 0;

					lua.retInt(m_nonce);
				} else {
					// Connection creation failed.
					lua.retBoolean(false);
				}
			}
		}
		break;
	}
	return 1;
}

bool
CKLBNetAPI::lua_callback(int msg, int status, CKLBJsonItem* pRoot, int nonce)
{
	return CKLBScriptEnv::getInstance().call_netAPI_callback(m_callback, this, nonce, msg, status, pRoot);
}
