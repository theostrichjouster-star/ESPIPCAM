// Provides web server for user control of app
// 
// s60sc 2022 - 2023

#include "appGlobals.h"

#define MAX_HANDLERS 12

char inFileName[IN_FILE_NAME_LEN];
static char variable[FILE_NAME_LEN]; 
static char value[IN_FILE_NAME_LEN]; 
static char retainAction[2];
int refreshVal = 5000; // msecs

static httpd_handle_t httpServer = NULL; // web server port
static int fdWs = -1; // websocket sockfd
static httpd_handle_t sseSocketHD; // SSE support
static int sseSocketFD;
bool useHttps = false;
bool useSecure = false;
bool heartBeatDone = false;

static byte* chunk;

esp_err_t sendChunks(File df, httpd_req_t *req, bool endChunking) {   
  // use chunked encoding to send large content to browser
  size_t chunksize = 0;
  esp_err_t res = ESP_OK;
  while ((chunksize = df.read(chunk, CHUNKSIZE))) {
    // a sustain download asked to stop (OTA teardown) must return promptly so the task
    // parks before endTasks() deletes it - see sustainCancelled()
    if (sustainCancelled()) {
      LOG_WRN("Download of %s cancelled", inFileName);
      res = ESP_FAIL;
      break;
    }
    res = httpd_resp_send_chunk(req, (char*)chunk, chunksize);
    if (res != ESP_OK) break;
    // httpd_sess_update_lru_counter(req->handle, httpd_req_to_sockfd(req));
  }
  if (endChunking) {
    df.close();
    httpd_resp_sendstr_chunk(req, NULL);
  }
  if (res != ESP_OK) LOG_WRN("Failed to send to browser: %s, err %s", inFileName, espErrMsg(res));
  return res;
}

esp_err_t fileHandler(httpd_req_t* req, bool download) {
  // send file contents to browser
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (!strcmp(inFileName, LOG_FILE_PATH)) flush_log(false);
  File df = STORAGE.open(inFileName);
  if (!df) {
    LOG_WRN("File does not exist or cannot be opened: %s", inFileName);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  } 
  if (!df.size()) {
    // file is empty
    df.close();
    httpd_resp_sendstr(req, NULL);
    return ESP_OK;
  }
  
  // Check if browser already has this version of the file.
  // The tag has to identify the file, not the firmware. It used to be CFG_VER, a compile time
  // constant untouched since upstream v8.3, so every file the board served shared one tag and
  // an edited data file was answered 304 for as long as the browser kept its copy. That is how
  // the 1280x960 option sat in MJPEG2SD.htm from ccf5e77 onwards without ever reaching a
  // browser, and why no web UI edit could be seen without clearing the cache by hand.
  // Size and last write time both come free from the open handle. They collide only when an
  // edit preserves the byte count AND the timestamp is unset, which needs the clock to have
  // been unset when the file was written - SD_MMC takes mtime from the system clock, so a file
  // fetched before the first NTP sync reads zero. Hashing the content would be exact but means
  // reading all 93KB of the page on every request, which is not worth it here
  char inVer[32];
  char fileVer[32];
  snprintf(fileVer, sizeof(fileVer), "%llx-%x", (unsigned long long)df.getLastWrite(), (unsigned)df.size());
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inVer, sizeof(inVer)) == ESP_OK) {
    if (!strcmp(inVer, fileVer)) {
      // already has this version cached, no need to resend
      httpd_resp_set_status(req, "304 Not Modified");
      return httpd_resp_send(req, NULL, 0);
    }
  }
  // this version not cached, so send it
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  // fileVer must outlive the response: httpd_resp_set_hdr keeps the pointer rather than copying
  // the string, and the send happens below in this same stack frame
  httpd_resp_set_hdr(req, "ETag", fileVer);
  return (download) ? downloadFile(df, req) : sendChunks(df, req);
}

static void displayLog(httpd_req_t *req) {
  // output ram log to browser
  if (logType == 0) {
    int startPtr, endPtr;
    startPtr = endPtr = mlogEnd;  
    httpd_resp_set_type(req, "text/plain"); 
    
    // output log in chunks
    do {
      int maxChunk = startPtr < endPtr ? endPtr - startPtr : RAM_LOG_LEN - startPtr;
      size_t chunkSize = std::min(CHUNKSIZE, maxChunk);    
      if (chunkSize > 0) httpd_resp_send_chunk(req, messageLog + startPtr, chunkSize); 
      startPtr += chunkSize;
      if (startPtr >= RAM_LOG_LEN) startPtr = 0;
    } while (startPtr != endPtr);
    httpd_resp_sendstr_chunk(req, NULL);
  } 
}

static bool constTimeEquals(const char* a, const char* b) {
  // constant-time string comparison to avoid leaking credential info via response timing
  size_t lenA = strlen(a), lenB = strlen(b);
  uint8_t diff = (lenA == lenB) ? 0 : 1;
  size_t maxLen = lenA > lenB ? lenA : lenB;
  for (size_t i = 0; i < maxLen; i++) {
    uint8_t ca = i < lenA ? (uint8_t)a[i] : 0;
    uint8_t cb = i < lenB ? (uint8_t)b[i] : 0;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

bool checkAuth(httpd_req_t* req) {
  // check if authentication is required
  if (strlen(Auth_Name)) {
    // authentication required
    size_t credLen = strlen(Auth_Name) + strlen(Auth_Pass) + 2; // +2 for colon & terminator
    char credentials[credLen];
    snprintf(credentials, credLen, "%s:%s", Auth_Name, Auth_Pass);
    const char* encodedCreds = encode64(credentials);
    size_t expectedLen = strlen("Basic ") + strlen(encodedCreds) + 1;
    char expectedAuth[expectedLen];
    snprintf(expectedAuth, expectedLen, "Basic %s", encodedCreds);
    size_t authHdrLen = httpd_req_get_hdr_value_len(req, "Authorization");
    bool authenticated = false;

    if (authHdrLen) {
      // check credentials supplied are valid
      size_t authLen = authHdrLen + 1;
      char auth[authLen];
      if (httpd_req_get_hdr_value_str(req, "Authorization", auth, authLen) == ESP_OK) {
        if (constTimeEquals(auth, expectedAuth)) authenticated = true;
      }
    }
    if (!authenticated) {
      // not authenticated
      httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic");
      httpd_resp_set_status(req, "401 Unauthorised");
      httpd_resp_sendstr(req, NULL);
      return false;
    }
  }
  return true; // authentication ok or not required
}

static esp_err_t indexHandler(httpd_req_t* req) {
  strcpy(inFileName, INDEX_PAGE_PATH);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  // first check if a startup failure needs to be reported
  if (strlen(startupFailure)) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, failPageS_html);
    httpd_resp_sendstr_chunk(req, startupFailure);
    httpd_resp_sendstr_chunk(req, failPageE_html);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
  } 
  // Show wifi wizard if not setup, using access point mode
  if (!STORAGE.exists(INDEX_PAGE_PATH) && WiFi.status() != WL_CONNECTED) {
    // Open a basic wifi setup page
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, setupPage_html);
  } else if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed

  return fileHandler(req);
}

esp_err_t extractHeaderVal(httpd_req_t *req, const char* variable, char* value) {
  // check if header field present, if so extract value
  esp_err_t res = ESP_FAIL;
  size_t hdrFieldLen = httpd_req_get_hdr_value_len(req, variable);
  if (!hdrFieldLen) return ESP_ERR_INVALID_ARG; // header not present
  else if (hdrFieldLen >= IN_FILE_NAME_LEN - 1) LOG_WRN("Field %s value too long (%d)", variable, hdrFieldLen);
  else {
    res = httpd_req_get_hdr_value_str(req, variable, value, hdrFieldLen + 1);
    if (res != ESP_OK) LOG_ERR("Value for %s could not be retrieved: %s", variable, espErrMsg(res));
  }
  return res;
}

esp_err_t extractQueryKeyVal(httpd_req_t *req, char* variable, char* value) {
  // get variable and value pair from URL query
  size_t queryLen = httpd_req_get_url_query_len(req) + 1;
  if (queryLen >= FILE_NAME_LEN) {
    LOG_WRN("Query string too long (%u)", queryLen);
    httpd_resp_set_status(req, "400 Query string too long");
    httpd_resp_sendstr(req, NULL);
    return ESP_FAIL;
  }
  httpd_req_get_url_query_str(req, variable, queryLen);
  urlDecode(variable);
  // extract key 
  char* endPtr = strchr(variable, '=');
  if (endPtr != NULL) {
    *endPtr = 0; // split variable into 2 strings, first is key name
    strcpy(value, endPtr + 1); // value is now second part of string
  } else {
    LOG_ERR("Invalid query string %s", variable);
    httpd_resp_set_status(req, "400 Invalid query string");
    httpd_resp_sendstr(req, NULL);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t webHandler(httpd_req_t* req) {
  // return required web page or component to browser using filename from query string
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  size_t queryLen = httpd_req_get_url_query_len(req) + 1;
  if (queryLen >= FILE_NAME_LEN) {
    LOG_WRN("Query string too long (%u)", queryLen);
    httpd_resp_set_status(req, "400 Query string too long");
    httpd_resp_sendstr(req, NULL);
    return ESP_FAIL;
  }
  httpd_req_get_url_query_str(req, variable, queryLen);
  urlDecode(variable);
  if (!pathIsSafe(variable)) {
    LOG_WRN("Rejected unsafe path in web request: %s", variable);
    httpd_resp_set_status(req, "400 Invalid path");
    httpd_resp_sendstr(req, NULL);
    return ESP_FAIL;
  }

  // check file extension to determine required processing before response sent to browser
  size_t varLen = strlen(variable);
  if (!strcmp(variable, "OTA.htm")) {
    // request for built in OTA page, if index html defective
    httpd_resp_set_type(req, "text/html"); 
    return httpd_resp_sendstr(req, otaPage_html);
  } else if (!strcmp(HTML_EXT, variable+(varLen-strlen(HTML_EXT)))) {
    // any other html file
    httpd_resp_set_type(req, "text/html");
  } else if (!strcmp(JS_EXT, variable+(varLen-strlen(JS_EXT)))) {
    // any js file
    httpd_resp_set_type(req, "text/javascript");
  } else if (!strcmp(CSS_EXT, variable+(varLen-strlen(CSS_EXT)))) {
    // any css file
    httpd_resp_set_type(req, "text/css");
  } else if (!strcmp(TEXT_EXT, variable+(varLen-strlen(TEXT_EXT)))) {
    // any text file
    httpd_resp_set_type(req, "text/plain");
  } else if (!strcmp(ICO_EXT, variable+(varLen-strlen(ICO_EXT)))) {
    // any icon file
    httpd_resp_set_type(req, "image/x-icon");
  } else if (!strcmp(SVG_EXT, variable+(varLen-strlen(SVG_EXT)))) {
    // any svg file
    httpd_resp_set_type(req, "image/svg+xml");
  } else LOG_WRN("Unknown file type %s", variable);  
  int dlen = snprintf(inFileName, IN_FILE_NAME_LEN - 1, "%s/%s", DATA_DIR, variable);               
  if (dlen >= IN_FILE_NAME_LEN) LOG_WRN("file name truncated");
  return fileHandler(req);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  // process control query from browser
  // obtain details from query string
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (extractQueryKeyVal(req, variable, value) != ESP_OK) return ESP_FAIL;
  if (!strcmp(variable, "displayLog")) displayLog(req);
  else {
    if (!strcmp(variable, "reset")) {
      httpd_resp_sendstr(req, NULL); // stop browser resending reset
      doRestart(value);
      return ESP_OK;
    }
    if (!strcmp(variable, "startOTA")) {
      if (!pathIsSafe(value)) {
        LOG_WRN("Rejected unsafe OTA file name: %s", value);
        httpd_resp_sendstr(req, NULL);
        return ESP_FAIL;
      }
      snprintf(inFileName, IN_FILE_NAME_LEN - 1, "%s/%s", DATA_DIR, value);
    }
    else {
      if (!strcmp(variable, "forceRecord")) {
        // Name the requester. A phantom forced recording starts ~90s after every boot
        // and again after each file-cap close, with every known browser closed - some
        // client is asking for it, and this line prints which one
        char peerIp[46] = "unknown";
        int sockFd = httpd_req_to_sockfd(req);
        struct sockaddr_in6 peerAddr;
        socklen_t addrLen = sizeof(peerAddr);
        if (sockFd >= 0 && getpeername(sockFd, (struct sockaddr*)&peerAddr, &addrLen) == 0)
          inet_ntop(AF_INET6, &peerAddr.sin6_addr, peerIp, sizeof(peerIp));
        LOG_ALT("forceRecord=%s requested by %s", value, peerIp);
      }
      // if not handled by appSpecificWebHandler(), try updateStatus()
      if (appSpecificWebHandler(req, variable, value) == ESP_FAIL) updateStatus(variable, value);
    }
  }
  httpd_resp_sendstr(req, NULL); 
  return ESP_OK;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  uint8_t filter = (uint8_t)httpd_req_get_url_query_len(req); // filter number is length of query string
  buildJsonString(filter);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, jsonBuff);
  return ESP_OK;
}

bool parseJson(int rxSize) {
  // process json in jsonBuff to extract properly formatted flat key:value pairs
  bool retAction = false;
  if (rxSize > JSON_BUFF_LEN) LOG_WRN("Received json too long %d", rxSize);
  else {
    jsonBuff[rxSize - 1] = ','; // replace final '}' 
    jsonBuff[rxSize] = 0; // terminator
    char* ptr = jsonBuff + 1; // skip over initial '{'
    size_t itemLen = 0; 
    do {
      // get and process each key:value in turn
      char* endItem = strchr(ptr += itemLen, ':');
      itemLen = endItem - ptr;
      memcpy(variable, ptr, itemLen);
      variable[itemLen] = 0;
      removeChar(variable, '"');
      ptr++;
      endItem = strchr(ptr += itemLen, ',');
      itemLen = endItem - ptr;
      memcpy(value, ptr, itemLen);
      value[itemLen] = 0;
      removeChar(value, '"');
      ptr++;
      if (!strcmp(variable, "action")) {
        strcpy(retainAction, value);
        retAction = true;
      } else updateStatus(variable, value);
    } while (ptr + itemLen - jsonBuff < rxSize);
  }
  return retAction;
}

static esp_err_t sseHandler(httpd_req_t *req) {
  // enable Server Sent Events
  const char* sseHeader = "HTTP/1.1 200 OK\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: keep-alive\r\n"
                          "Content-Type: text/event-stream\r\n\r\n";
  sseSocketHD = req->handle;
  sseSocketFD = httpd_req_to_sockfd(req);
  httpd_socket_send(sseSocketHD, sseSocketFD, sseHeader, strlen(sseHeader), 0); 
  sendSSE("open", "opened");
  return ESP_OK;
}

#define SSESEP "\r\n\r\n" // SSE event separator
void sendSSE(const char* eventType, const char* eventData) {
  // send event data to browser
  if (sseSocketFD > 0) {
    char eventMsg[30];
    snprintf(eventMsg, 30 - 1, "event: %s\ndata: ", eventType);
    int res = httpd_socket_send(sseSocketHD, sseSocketFD, eventMsg, strlen(eventMsg), 0);
    res = httpd_socket_send(sseSocketHD, sseSocketFD, eventData, strlen(eventData), 0);
    res = httpd_socket_send(sseSocketHD, sseSocketFD, SSESEP, strlen(SSESEP), 0);
    if (res == HTTPD_SOCK_ERR_TIMEOUT) LOG_WRN("Timeout/interrupted while using socket");
    if (res == HTTPD_SOCK_ERR_FAIL) LOG_WRN("Unrecoverable error while using socket");
    if (res == HTTPD_SOCK_ERR_INVALID) LOG_WRN("Invalid arguments %s, %s", eventType, eventData);
  } else LOG_ERR("SSE not initiated");
}

static esp_err_t updateHandler(httpd_req_t *req) {
  // bulk update of config, extract key pairs from received json string
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  size_t rxSize = min(req->content_len, (size_t)JSON_BUFF_LEN);
  int ret = 0;
  // obtain json payload
  do {
    ret = httpd_req_recv(req, jsonBuff, rxSize);
    if (ret < 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
      else LOG_WRN("Update request failed with status %i", ret);
    }
  } while (ret > 0);
  httpd_resp_sendstr(req, NULL); 
  if (ret >= 0 && parseJson(rxSize)) appSpecificWebHandler(req, "action", retainAction); 
  return ret < 0 ? ESP_FAIL : ESP_OK;
}

void progress(size_t prg, size_t sz) {
  static uint8_t pcProgress = 0;
  if (calcProgress(prg, sz, 5, pcProgress)) LOG_INF("OTA uploaded %d%%", pcProgress); 
}

esp_err_t uploadHandler(httpd_req_t *req) {
  // upload file for storage or firmware update
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  esp_err_t res = ESP_OK;
  size_t fileSize = req->content_len;
  size_t rxSize = min(fileSize, (size_t)JSON_BUFF_LEN);
  int bytesRead = -1;
  LOG_INF("Upload file %s", inFileName);

  if (strstr(inFileName, ".bin") != NULL) {
    // partition update - sketch or SPIFFS
    if (fileSize < MIN_OTA_IMAGE_SIZE) {
      LOG_WRN("Rejected OTA upload %s, implausibly small (%u bytes)", inFileName, fileSize);
      httpd_resp_set_status(req, "400 File too small to be a valid firmware image");
      httpd_resp_sendstr(req, NULL);
      return ESP_FAIL;
    }
    LOG_INF("Firmware update using file %s", inFileName);
    OTAprereq();
    if (fdWs >= 0) httpd_sess_trigger_close(httpServer, fdWs);
    // a spiffs binary must have 'spiffs' in the filename
    int cmd = (strstr(inFileName, "spiffs") != NULL) ? U_SPIFFS : U_FLASH;
    if (cmd == U_SPIFFS) STORAGE.end(); // close relevant file system
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      // report and return rather than rebooting - restarting achieves nothing here and
      // discards the reason the update could not start
      LOG_WRN("OTA could not start: %s", Update.errorString());
      httpd_resp_set_status(req, "500 OTA could not start");
      httpd_resp_sendstr(req, NULL);
      return ESP_FAIL;
    }
    Update.onProgress(progress); // once, not per chunk
    {
      // The timeout arm below must be bounded. A half open socket - sender vanished mid
      // transfer, no RST ever delivered - makes httpd_req_recv() return TIMEOUT indefinitely,
      // and an unbounded retry held the only httpd worker in this loop with the tasks and
      // watchdog already torn down by OTAprereq(): the board answered pings for ever and never
      // served HTTP again. A clean disconnect errors and breaks; only the stall had no exit
      uint32_t lastData = millis();
      do {
        bytesRead = httpd_req_recv(req, jsonBuff, rxSize);
        if (bytesRead < 0) {
          if (bytesRead == HTTPD_SOCK_ERR_TIMEOUT) {
            if (millis() - lastData > 30000) {
              LOG_WRN("OTA upload stalled for 30s with %u bytes outstanding", fileSize);
              break;
            }
            delay(10);
            continue;
          } else {
            LOG_WRN("Upload request failed with status %i", bytesRead);
            break;
          }
        }
        lastData = millis();
        // a short write means the flash write failed - carrying on would just build a
        // corrupt image and report success from the byte count
        if (Update.write((uint8_t*)jsonBuff, (size_t)bytesRead) != (size_t)bytesRead) {
          LOG_WRN("OTA flash write failed: %s", Update.errorString());
          break;
        }
        fileSize -= bytesRead;
      } while (bytesRead > 0);
      if (!fileSize) Update.end(true); // true to set the size to the current progress
      else Update.abort(); // release the handle rather than leaving it mid-update
    }
    // an aborted transfer leaves bytes outstanding without any Update error, so success is
    // completion, not merely the absence of a write failure. The restart runs either way -
    // an incomplete image sits in the inactive slot and is discarded by rebooting the active one
    bool otaOk = !Update.hasError() && !fileSize;
    if (otaOk) LOG_INF("OTA update complete for %s", cmd == U_FLASH ? "Sketch" : "SPIFFS");
    else LOG_WRN("OTA failed: %s", Update.hasError() ? Update.errorString() : "transfer incomplete");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, otaOk ? "OTA update complete, restarting ..." : "OTA update failed, restarting ...");
    doRestart("Restart after OTA");

  } else {
    // create / replace data file on storage
    File uf = STORAGE.open(inFileName, FILE_WRITE);
    if (!uf) LOG_WRN("Failed to open %s on storage", inFileName);
    else {
      // obtain file content. Same stall bound as the OTA branch: a half open socket returns
      // TIMEOUT for ever and would hold the only httpd worker - here the board is otherwise
      // intact, so breaking with a failure is enough and no restart is needed
      uint32_t lastData = millis();
      do {
        bytesRead = httpd_req_recv(req, jsonBuff, rxSize);
        if (bytesRead < 0) {
          if (bytesRead == HTTPD_SOCK_ERR_TIMEOUT) {
            if (millis() - lastData > 30000) {
              LOG_WRN("Upload of %s stalled for 30s, abandoned", inFileName);
              bytesRead = -1;
              break;
            }
            delay(10);
            continue;
          } else {
            LOG_WRN("Upload request failed with status %i", bytesRead);
            break;
          }
        }
        lastData = millis();
        uf.write((const uint8_t*)jsonBuff, bytesRead);
      } while (bytesRead > 0);
      uf.close();
      res = bytesRead < 0 ? ESP_FAIL : ESP_OK;
      httpd_resp_sendstr(req, res == ESP_OK ? "Completed upload file" : "Failed to upload file, retry");
      if (res == ESP_OK) LOG_INF("Uploaded file %s", inFileName);
      else LOG_WRN("Failed to upload file %s", inFileName);     
    }
  }
  return res;
}

static esp_err_t setupHandler(httpd_req_t *req) {
  if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
  // Scan for WiFi networks
  int w = WiFi.scanNetworks();
  // Start building the JSON string
  char* p = jsonBuff;
  p += sprintf(p, "{\"networks\":[");
  // Populate the JSON string with scan results
  for (int i = 0; i < w; ++i) {
    p += sprintf(p, "{\"ssid\":\"%s\",\"encryption\":\"%s\",\"strength\":\"%ld\"},", WiFi.SSID(i).c_str(), getEncType(i), WiFi.RSSI(i));
  }
  // remove final comma and close the JSON array
  if (w > 0) p += sprintf(p-1, "]}");
  // Set the response type to JSON and send JSON
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_sendstr(req, jsonBuff);
  return ESP_OK;
}

static esp_err_t sendCrossOriginHeader(httpd_req_t *req) {
  // prevent CORS from blocking request
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST,GET,HEAD,OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
  httpd_resp_set_status(req, "204");
  httpd_resp_sendstr(req, NULL); 
  return ESP_OK;
}

static bool checkWsSocketStatus() {
  // Check if connection is active and is a WebSocket
  return (httpd_ws_get_fd_info(httpServer, fdWs) == HTTPD_WS_CLIENT_WEBSOCKET) ? true : false;
}

bool wsAsyncSendText(const char* wsData) {
  // websockets send text function, used for async logging and status updates
  if (checkWsSocketStatus()) {
    // send if connection active
    httpd_ws_frame_t wsPkt;
    wsPkt.payload = (uint8_t*)wsData;
    wsPkt.len = strlen(wsData);
    wsPkt.type = HTTPD_WS_TYPE_TEXT;
    wsPkt.final = true;
    esp_err_t ret = httpd_ws_send_frame_async(httpServer, fdWs, &wsPkt);
    if (ret != ESP_OK) LOG_WRN("websocket send failed with %s", esp_err_to_name(ret));
    return ret == ESP_OK ? true : false;
  } 
  return false;
}

bool wsAsyncSendJson(const char* dataType, const char* wsData) {
  // build json to send
  char wsJson[strlen(dataType) + strlen(wsData) + 30];
  sprintf(wsJson, "{\"type\":\"%s\",\"payload\":{%s}}", dataType, wsData);
  return wsAsyncSendText(wsJson);
}

void wsAsyncSendBinary(uint8_t* data, size_t len) {
  // websockets send binary function, used for app specific features
  if (checkWsSocketStatus()) {
    if (data == NULL || len == 0) {
      LOG_WRN("Invalid data or length: data=%p, len=%u", data, len);
      return;
    }
    // send if connection active
    httpd_ws_frame_t wsPkt;
    memset(&wsPkt, 0, sizeof(httpd_ws_frame_t)); // Initialize all fields to zero
    wsPkt.type = HTTPD_WS_TYPE_BINARY;
    wsPkt.payload = data;
    wsPkt.len = len;
    esp_err_t ret = httpd_ws_send_frame_async(httpServer, fdWs, &wsPkt);
    if (ret != ESP_OK) LOG_WRN("websocket send failed with %s", esp_err_to_name(ret));
  } // else ignore
}

static esp_err_t wsHandler(httpd_req_t *req) {
  // receive websocket data and determine response
  // if a new connection is received, the old connection is closed, but the browser
  // page on the newer connection may need to be manually refreshed to take over the log
  esp_err_t ret = ESP_OK;
  if (req->method == HTTP_GET) {
    // websocket connection request from browser client
    // the websocket carries the same config-update/control commands as /control and /update
    // (see appSpecificWsHandler cases 'C'/'U'), so it needs the same auth check on connection
    if (!checkAuth(req)) return ESP_OK;
    if (fdWs != -1) {
      if (fdWs != httpd_req_to_sockfd(req)) {
        // websocket connection from browser when another browser connection is active
        LOG_VRB("closing connection, as newer Websocket on %u", httpd_req_to_sockfd(req));
        // kill older connection
        killSocket();
      }
    }
    fdWs = httpd_req_to_sockfd(req);
    if (fdWs < 0) {
      LOG_WRN("failed to get socket number");
      ret = ESP_FAIL;
    } else LOG_VRB("Websocket connection: %d", fdWs);
  } else {
    // data content received
    httpd_ws_frame_t wsPkt;
    uint8_t wsMsg[MAX_PAYLOAD_LEN];
    memset(&wsPkt, 0, sizeof(httpd_ws_frame_t));
    wsPkt.payload = wsMsg;
    ret = httpd_ws_recv_frame(req, &wsPkt, MAX_PAYLOAD_LEN); 
    if (ret == ESP_OK) {
      if (wsPkt.len >= MAX_PAYLOAD_LEN) LOG_ERR("websocket payload too long %d", wsPkt.len);
      wsMsg[wsPkt.len] = 0; // terminator
      if (wsPkt.type == HTTPD_WS_TYPE_BINARY && wsPkt.len) appSpecificWsBinHandler(wsMsg, wsPkt.len);
      else if (wsPkt.type == HTTPD_WS_TYPE_TEXT) appSpecificWsHandler((const char*)wsMsg);
      else if (wsPkt.type == HTTPD_WS_TYPE_CLOSE) appSpecificWsHandler("X");
    } else LOG_ERR("websocket receive failed with %s", esp_err_to_name(ret));
  }
  return ret;
}

void killSocket(int skt) {
  // user requested
  if (skt == -99) {
    skt = fdWs;
    fdWs = -1;
  }
  if (skt >= 0) {
    httpd_sess_trigger_close(httpServer, skt);
    delay(10);
  }
}

/*
static void https_server_user_callback(esp_https_server_user_cb_arg_t *user_cb) {
  LOG_DBG("Session created, socket: %d", user_cb->tls->sockfd);
}
*/

static esp_err_t customOrNotFoundHandler(httpd_req_t *req, httpd_err_code_t err) {
  // either handle WebDAV methods or report non existent URI
  if (req->method == HTTP_OPTIONS) sendCrossOriginHeader(req);
#if INCLUDE_WEBDAV
  if (strncmp(req->uri, WEBDAV, strlen(WEBDAV)) == 0) {
    if (!checkAuth(req)) return ESP_OK; // check if authentication required & passed
    return handleWebDav(req) ? ESP_OK : ESP_FAIL;
  }
#endif
  // For any other URI send 404 and close socket
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

bool startWebServer() {
  esp_err_t res = ESP_FAIL;
  if (!chunk) chunk = psramFound() ? (byte*)ps_malloc(CHUNKSIZE) : (byte*)malloc(CHUNKSIZE);
  if (httpServer) {
    httpd_stop(httpServer);
    httpServer = NULL;
  }
  
#if INCLUDE_CERTS
  if (useHttps) {
    // HTTPS server
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32S3
    config.httpd.stack_size = SERVER_STACK_SIZE;
#endif  
    config.prvtkey_pem = (const uint8_t*)serverCerts[0];
    config.prvtkey_len = strlen(serverCerts[0]) + 1;
    config.servercert = (const uint8_t*)serverCerts[1];
    config.servercert_len = strlen(serverCerts[1]) + 1;
  
    //config.user_cb = https_server_user_callback;
    config.httpd.server_port = HTTPS_PORT;
    config.httpd.lru_purge_enable = true; // close least used socket 
    config.httpd.max_uri_handlers = MAX_HANDLERS;
    config.httpd.max_open_sockets = HTTP_CLIENTS + MAX_STREAMS;
    config.httpd.task_priority = HTTP_PRI;
    //config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    res = httpd_ssl_start(&httpServer, &config);
  } else {
#else
  if (true) {
#endif
    // HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
#if CONFIG_IDF_TARGET_ESP32S3
    config.stack_size = SERVER_STACK_SIZE;
#endif
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = MAX_HANDLERS;
    config.max_open_sockets = HTTP_CLIENTS + MAX_STREAMS;
    config.task_priority = HTTP_PRI;
    //config.uri_match_fn = httpd_uri_match_wildcard;
    res = httpd_start(&httpServer, &config);
  }
  httpd_uri_t indexUri = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL};
  httpd_uri_t webUri = {.uri = "/web", .method = HTTP_GET, .handler = webHandler, .user_ctx = NULL};
  httpd_uri_t controlUri = {.uri = "/control", .method = HTTP_GET, .handler = controlHandler, .user_ctx = NULL};
  httpd_uri_t updateUri = {.uri = "/update", .method = HTTP_POST, .handler = updateHandler, .user_ctx = NULL};
  httpd_uri_t statusUri = {.uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = NULL};
  httpd_uri_t uploadUri = {.uri = "/upload", .method = HTTP_POST, .handler = uploadHandler, .user_ctx = NULL};
  httpd_uri_t wifiUri = {.uri = "/wifi", .method = HTTP_GET, .handler = setupHandler, .user_ctx = NULL};
  httpd_uri_t sseUri = {.uri = "/sse", .method = HTTP_GET, .handler = sseHandler, .user_ctx = NULL};
  httpd_uri_t wsUri = {.uri = "/ws", .method = HTTP_GET, .handler = wsHandler, .user_ctx = NULL, .is_websocket = true};
  httpd_uri_t sustainUri = {.uri = "/sustain", .method = HTTP_GET, .handler = appSpecificSustainHandler, .user_ctx = NULL};
  httpd_uri_t checkUri = {.uri = "/sustain", .method = HTTP_HEAD, .handler = appSpecificSustainHandler, .user_ctx = NULL};

  if (res == ESP_OK) {
    // Registration returns were silently discarded, which turned a missing route into an
    // unexplainable mystery: on 30 Aug the root page 404'd for an entire boot while every
    // other route worked, and nothing recorded why. A failed registration now names the
    // route and the reason, so the log convicts the culprit immediately
    const httpd_uri_t* uris[] = {&indexUri, &webUri, &controlUri, &updateUri, &statusUri,
      &uploadUri, &sseUri, &wifiUri, &wsUri, &sustainUri, &checkUri};
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
      esp_err_t regRes = httpd_register_uri_handler(httpServer, uris[i]);
      if (regRes != ESP_OK) LOG_WRN("Failed to register %s handler: %s", uris[i]->uri, espErrMsg(regRes));
    }
    esp_err_t errRes = httpd_register_err_handler(httpServer, HTTPD_404_NOT_FOUND, customOrNotFoundHandler);
    if (errRes != ESP_OK) LOG_WRN("Failed to register 404 handler: %s", espErrMsg(errRes));

    LOG_INF("Starting web server on port: %u", useHttps ? HTTPS_PORT : HTTP_PORT);
    LOG_INF("Remote server certificates %s checked", useSecure ? "are" : "not");
    if (DEBUG_MEM) {
      uint32_t freeStack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
      LOG_INF("Task httpServer stack space %lu", freeStack);
    }
  } else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to start webserver %s", espErrMsg(res));
  if (!DBG_ON) esp_log_level_set("*", ESP_LOG_NONE); // suppress ESP_LOG_ERROR messages
  debugMemory("startWebserver");
  if (strlen(startupFailure)) {
    LOG_WRN("%s", startupFailure);
    return false;
  }
  return true;
}
