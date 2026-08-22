// Check for and apply firmware updates published as GitHub Releases.
//
// Two steps, both triggered from the web UI and both behind the same
// authentication as every other /control action:
//   /control?checkUpdate=1 - ask the GitHub API what the latest release is
//   /control?doUpdate=1    - download that release's asset and flash it
//
// Reuses the HTTPClient / NetworkClientSecure pattern from wgetFile() in
// setupAssist.cpp and the Update.begin/write/end sequence from uploadHandler()
// in webServer.cpp, so no extra library dependency is introduced.

#include "appGlobals.h"

char otaLatestTag[OTA_TAG_LEN] = "";        // release tag exactly as published, eg "v1.0.1"
char otaStatus[OTA_STATUS_LEN] = "Not checked";
bool otaUpdateAvailable = false;
static TaskHandle_t otaHandle = NULL;
static bool otaInProgress = false;

// Parse a dotted version into its parts, tolerating a leading 'v' and a
// missing minor/patch ("v2" is 2.0.0). Returns false if there is no leading
// number at all, so garbage tags are rejected rather than read as 0.0.0.
static bool parseVer(const char* ver, int& major, int& minor, int& patch) {
  while (*ver == 'v' || *ver == 'V') ver++;
  if (*ver < '0' || *ver > '9') return false;
  major = minor = patch = 0;
  return sscanf(ver, "%d.%d.%d", &major, &minor, &patch) >= 1;
}

// >0 if a is newer than b, 0 if equal or either is unparseable
static int compareVer(const char* a, const char* b) {
  int aMaj, aMin, aPat, bMaj, bMin, bPat;
  if (!parseVer(a, aMaj, aMin, aPat) || !parseVer(b, bMaj, bMin, bPat)) return 0;
  if (aMaj != bMaj) return aMaj - bMaj;
  if (aMin != bMin) return aMin - bMin;
  return aPat - bPat;
}

bool checkForUpdate() {
  // Ask GitHub for the latest release and compare its tag against APP_VER
  if (otaInProgress) {
    LOG_WRN("Update already in progress");
    return false;
  }
  otaUpdateAvailable = false;
  otaLatestTag[0] = 0;
  snprintf(otaStatus, OTA_STATUS_LEN, "Checking ...");

  NetworkClientSecure wclient;
  if (!remoteServerConnect(wclient, GITHUB_API_HOST, HTTPS_PORT, git_rootCACertificate, OTAGITHUB)) {
    snprintf(otaStatus, OTA_STATUS_LEN, "Could not reach GitHub");
    return false;
  }

  bool res = false;
  char apiPath[128];
  snprintf(apiPath, sizeof(apiPath), "/repos/%s/%s/releases/latest", OTA_REPO_OWNER, OTA_REPO_NAME);
  HTTPClient https;
  if (https.begin(wclient, GITHUB_API_HOST, HTTPS_PORT, apiPath)) {
    // the GitHub API rejects requests without a User-Agent
    https.setUserAgent(APP_NAME);
    https.addHeader("Accept", "application/vnd.github+json");
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      char tag[FILE_NAME_LEN] = "";
      if (getJsonValue(payload.c_str(), "tag_name", tag) && strlen(tag)) {
        strncpy(otaLatestTag, tag, OTA_TAG_LEN - 1);
        otaLatestTag[OTA_TAG_LEN - 1] = 0;
        int diff = compareVer(otaLatestTag, APP_VER);
        if (diff > 0) {
          otaUpdateAvailable = true;
          snprintf(otaStatus, OTA_STATUS_LEN, "Update %s available", otaLatestTag);
          LOG_INF("Update available: %s (running %s)", otaLatestTag, APP_VER);
        } else {
          snprintf(otaStatus, OTA_STATUS_LEN, "Up to date (%s)", APP_VER);
          LOG_INF("No update, latest release %s vs running %s", otaLatestTag, APP_VER);
        }
        res = true;
      } else {
        snprintf(otaStatus, OTA_STATUS_LEN, "Release has no tag_name");
        LOG_WRN("Could not read tag_name from GitHub response");
      }
    } else if (httpCode == HTTP_CODE_NOT_FOUND) {
      // the API returns 404, not an empty object, when a repo has no releases
      snprintf(otaStatus, OTA_STATUS_LEN, "No releases published yet");
      LOG_INF("No releases published for %s/%s", OTA_REPO_OWNER, OTA_REPO_NAME);
      res = true;
    } else if (httpCode == HTTP_CODE_FORBIDDEN) {
      // unauthenticated API calls are rate limited to 60/hour per IP
      snprintf(otaStatus, OTA_STATUS_LEN, "GitHub rate limit reached, retry later");
      LOG_WRN("GitHub API returned 403, likely rate limited");
    } else {
      snprintf(otaStatus, OTA_STATUS_LEN, "Check failed (HTTP %d)", httpCode);
      LOG_WRN("Update check failed, HTTP %d: %s", httpCode, https.errorToString(httpCode).c_str());
    }
    https.end();
  } else snprintf(otaStatus, OTA_STATUS_LEN, "Could not start GitHub request");
  remoteServerClose(wclient);
  return res;
}

static void otaTask(void* parameter) {
  // Download the release asset and write it straight into the OTA partition.
  // Runs as its own task so the web server is not blocked for the duration.
  char dlPath[192];
  snprintf(dlPath, sizeof(dlPath), "/%s/%s/releases/download/%s/%s",
    OTA_REPO_OWNER, OTA_REPO_NAME, otaLatestTag, OTA_ASSET_NAME);

  NetworkClientSecure wclient;
  HTTPClient https;
  bool started = false; // whether Update.begin() succeeded, so cleanup knows to abort

  if (!remoteServerConnect(wclient, GITHUB_DL_HOST, HTTPS_PORT, git_rootCACertificate, OTAGITHUB)) {
    snprintf(otaStatus, OTA_STATUS_LEN, "Could not reach GitHub");
  } else if (!https.begin(wclient, GITHUB_DL_HOST, HTTPS_PORT, dlPath)) {
    snprintf(otaStatus, OTA_STATUS_LEN, "Could not start download");
  } else {
    https.setUserAgent(APP_NAME);
    // release assets 302 to objects.githubusercontent.com
    https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    LOG_INF("Downloading %s from %s", OTA_ASSET_NAME, dlPath);
    int httpCode = https.GET();
    int contentLen = https.getSize();

    if (httpCode != HTTP_CODE_OK) {
      snprintf(otaStatus, OTA_STATUS_LEN, "Download failed (HTTP %d)", httpCode);
      LOG_WRN("Asset download failed, HTTP %d - is the release asset named %s?", httpCode, OTA_ASSET_NAME);
    } else if (contentLen < (int)MIN_OTA_IMAGE_SIZE) {
      // same guard as the manual upload path, so a truncated or error body
      // is never handed to Update.begin()
      snprintf(otaStatus, OTA_STATUS_LEN, "Image too small (%d bytes)", contentLen);
      LOG_WRN("Rejected OTA asset, implausibly small (%d bytes)", contentLen);
    } else {
      OTAprereq(); // shut down camera / streaming tasks before flashing
      if (!Update.begin(contentLen, U_FLASH)) {
        snprintf(otaStatus, OTA_STATUS_LEN, "Cannot start update: %s", Update.errorString());
        LOG_WRN("Update.begin failed: %s", Update.errorString());
      } else {
        started = true;
        size_t written = Update.writeStream(https.getStream());
        if (written != (size_t)contentLen) {
          snprintf(otaStatus, OTA_STATUS_LEN, "Download truncated");
          LOG_WRN("OTA wrote %u of %d bytes", written, contentLen);
        } else if (!Update.end(true)) {
          snprintf(otaStatus, OTA_STATUS_LEN, "Update failed: %s", Update.errorString());
          LOG_WRN("Update.end failed: %s", Update.errorString());
        } else {
          started = false; // completed, nothing to abort
          snprintf(otaStatus, OTA_STATUS_LEN, "Updated to %s, restarting", otaLatestTag);
          LOG_INF("OTA update to %s complete", otaLatestTag);
          https.end();
          remoteServerClose(wclient);
          otaInProgress = false;
          otaHandle = NULL;
          doRestart("Restart after GitHub OTA update");
          vTaskDelete(NULL);
          return;
        }
      }
    }
    if (started) Update.abort(); // leave the running partition intact on any partial write
    https.end();
  }
  remoteServerClose(wclient);
  LOG_WRN("OTA update did not complete: %s", otaStatus);
  otaInProgress = false;
  otaHandle = NULL;
  vTaskDelete(NULL);
}

bool startOtaUpdate() {
  // Kick off the download, having first confirmed there is something to download
  if (otaInProgress) {
    LOG_WRN("Update already in progress");
    return false;
  }
  if (!otaUpdateAvailable || !strlen(otaLatestTag)) {
    snprintf(otaStatus, OTA_STATUS_LEN, "Run a check first");
    LOG_WRN("Update requested but no newer release identified");
    return false;
  }
  otaInProgress = true;
  snprintf(otaStatus, OTA_STATUS_LEN, "Downloading %s ...", otaLatestTag);
  if (otaHandle == NULL)
    xTaskCreateWithCaps(&otaTask, "otaTask", OTA_STACK_SIZE, NULL, FTP_PRI, &otaHandle, STACK_MEM);
  return true;
}
