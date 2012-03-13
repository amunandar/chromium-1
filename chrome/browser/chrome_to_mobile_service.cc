// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chrome_to_mobile_service.h"

#include "base/bind.h"
#include "base/json/json_writer.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/printing/cloud_print/cloud_print_url.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/token_service.h"
#include "chrome/browser/signin/token_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/common/cloud_print/cloud_print_helpers.h"
#include "chrome/common/guid.h"
#include "chrome/common/net/gaia/gaia_urls.h"
#include "chrome/common/net/gaia/oauth2_access_token_fetcher.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_fetcher.h"
#include "net/base/escape.h"
#include "net/base/load_flags.h"
#include "net/url_request/url_request_context_getter.h"

namespace {

// The maximum number of retries for the URLFetcher requests.
size_t kMaxRetries = 10;

// Seconds between automatically retrying authentication (on failure).
int kAuthRetryDelay = 30;

// Seconds between automatically updating the mobile device list.
int kAutoSearchRetryDelay = 300;

// The cloud print Oath2 scope and 'printer' type of compatible mobile devices.
const char kOAuth2Scope[] = "https://www.googleapis.com/auth/cloudprint";
const char kTypeAndroidChromeSnapshot[] = "ANDROID_CHROME_SNAPSHOT";

// The types of Chrome To Mobile requests sent to the cloud print service.
const char kRequestTypeURL[] = "url";
const char kRequestTypeDelayedSnapshot[] = "url_with_delayed_snapshot";
const char kRequestTypeSnapshot[] = "snapshot";

// The snapshot path constants; used with a guid for each MHTML snapshot file.
const FilePath::CharType kSnapshotPath[] =
    FILE_PATH_LITERAL("chrome_to_mobile_snapshot_.mht");

// Get the "__c2dm__job_data" tag JSON data for the cloud print job submission.
std::string GetJobString(const ChromeToMobileService::RequestData& data) {
  scoped_ptr<DictionaryValue> job(new DictionaryValue());
  job->SetString("url", data.url.spec());
  if (data.type == ChromeToMobileService::URL) {
    job->SetString("type", kRequestTypeURL);
  } else {
    job->SetString("snapID", data.snapshot_id);
    job->SetString("type", (data.type == ChromeToMobileService::SNAPSHOT) ?
        kRequestTypeSnapshot : kRequestTypeDelayedSnapshot);
  }
  std::string job_string;
  base::JSONWriter::Write(job.get(), false, &job_string);
  return job_string;
}

// Get the URL for cloud print job submission; appends query params if needed.
GURL GetSubmitURL(GURL service_url,
                  const ChromeToMobileService::RequestData& data) {
  GURL submit_url = cloud_print::GetUrlForSubmit(service_url);
  if (data.type == ChromeToMobileService::SNAPSHOT)
    return submit_url;

  // Append form data to the URL's query for |URL| and |DELAYED_SNAPSHOT| jobs.
  static const bool kUsePlus = true;
  std::string tag_string = net::EscapeQueryParamValue(
      "__c2dm__job_data=" + GetJobString(data), kUsePlus);
  GURL::Replacements replacements;
  // Provide dummy content to workaround |errorCode| 412 'Document missing'.
  std::string query = StringPrintf("printerid=%s&tag=%s&title=%s"
      "&contentType=text/plain&content=dummy",
      net::EscapeQueryParamValue(UTF16ToUTF8(data.mobile_id), kUsePlus).c_str(),
      net::EscapeQueryParamValue(tag_string, kUsePlus).c_str(),
      net::EscapeQueryParamValue(UTF16ToUTF8(data.title), kUsePlus).c_str());
  replacements.SetQueryStr(query);
  return submit_url.ReplaceComponents(replacements);
}

// Delete the specified file; called as a BlockingPoolTask.
void DeleteFilePath(const FilePath& file_path) {
  bool success = file_util::Delete(file_path, false);
  DCHECK(success);
}

// Construct POST data and submit the MHTML snapshot file; deletes the snapshot.
void SubmitSnapshot(content::URLFetcher* request,
                    const ChromeToMobileService::RequestData& data) {
  std::string file;
  if (file_util::ReadFileToString(data.snapshot_path, &file) && !file.empty()) {
    std::string post_data, mime_boundary;
    cloud_print::CreateMimeBoundaryForUpload(&mime_boundary);
    cloud_print::AddMultipartValueForUpload("printerid",
        UTF16ToUTF8(data.mobile_id), mime_boundary, std::string(), &post_data);
    cloud_print::AddMultipartValueForUpload("tag", "__c2dm__job_data=" +
        GetJobString(data), mime_boundary, std::string(), &post_data);
    cloud_print::AddMultipartValueForUpload("title", UTF16ToUTF8(data.title),
        mime_boundary, std::string(), &post_data);
    cloud_print::AddMultipartValueForUpload("contentType", "multipart/related",
        mime_boundary, std::string(), &post_data);

    // Append the snapshot MHTML content and terminate the request body.
    post_data.append("--" + mime_boundary + "\r\n"
        "Content-Disposition: form-data; "
        "name=\"content\"; filename=\"blob\"\r\n"
        "Content-Type: text/mhtml\r\n"
        "\r\n" + file + "\r\n" "--" + mime_boundary + "--\r\n");
    std::string content_type = "multipart/form-data; boundary=" + mime_boundary;
    request->SetUploadData(content_type, post_data);
    request->Start();
  }

  content::BrowserThread::PostBlockingPoolTask(FROM_HERE,
      base::Bind(&DeleteFilePath, data.snapshot_path));
}

}  // namespace

ChromeToMobileService::Observer::~Observer() {}

ChromeToMobileService::RequestData::RequestData() {}

ChromeToMobileService::RequestData::~RequestData() {}

ChromeToMobileService::ChromeToMobileService(Profile* profile)
    : profile_(profile),
      cloud_print_url_(new CloudPrintURL(profile)),
      oauth2_retry_count_(0) {
  content::BrowserThread::PostBlockingPoolTask(FROM_HERE,
      base::Bind(&ChromeToMobileService::CreateUniqueTempDir,
                 base::Unretained(this)));
  RequestMobileListUpdate();
}

ChromeToMobileService::~ChromeToMobileService() {}

void ChromeToMobileService::OnURLFetchComplete(
    const content::URLFetcher* source) {
  if (source == search_request_.get())
    HandleSearchResponse();
  else
    HandleSubmitResponse(source);
}

void ChromeToMobileService::OnGetTokenSuccess(
    const std::string& access_token) {
  DCHECK(!access_token.empty());
  oauth2_request_.reset();
  request_timer_.Stop();
  oauth2_token_ = access_token;
  RequestMobileListUpdate();
}

void ChromeToMobileService::OnGetTokenFailure(
    const GoogleServiceAuthError& error) {
  oauth2_request_.reset();
  if (request_timer_.IsRunning() || (oauth2_retry_count_++ > kMaxRetries))
    return;
  request_timer_.Start(FROM_HERE, base::TimeDelta::FromSeconds(kAuthRetryDelay),
                       this, &ChromeToMobileService::RequestAuth);
}

void ChromeToMobileService::RequestMobileListUpdate() {
  if (oauth2_token_.empty())
    RequestAuth();
  else
    RequestSearch();
}

void ChromeToMobileService::GenerateSnapshot(base::WeakPtr<Observer> observer) {
  FilePath path(temp_dir_.path().Append(kSnapshotPath));
    BrowserList::GetLastActiveWithProfile(profile_)->GetSelectedWebContents()->
      GenerateMHTML(path.InsertBeforeExtensionASCII(guid::GenerateGUID()),
                    base::Bind(&Observer::SnapshotGenerated, observer));
}

void ChromeToMobileService::SendToMobile(const string16& mobile_id,
                                         const FilePath& snapshot,
                                         base::WeakPtr<Observer> observer) {
  DCHECK(!oauth2_token_.empty());
  RequestData data;
  data.mobile_id = mobile_id;
  content::WebContents* web_contents =
      BrowserList::GetLastActiveWithProfile(profile_)->GetSelectedWebContents();
  data.url = web_contents->GetURL();
  data.title = web_contents->GetTitle();
  data.snapshot_path = snapshot;
  bool send_snapshot = !snapshot.empty();
  data.snapshot_id = send_snapshot ? guid::GenerateGUID() : std::string();
  data.type = send_snapshot ? DELAYED_SNAPSHOT : URL;

  content::URLFetcher* submit_url = CreateRequest(data);
  request_observer_map_[submit_url] = observer;
  submit_url->Start();

  if (send_snapshot) {
    data.type = SNAPSHOT;
    content::URLFetcher* submit_snapshot = CreateRequest(data);
    request_observer_map_[submit_snapshot] = observer;
    content::BrowserThread::PostBlockingPoolTask(FROM_HERE,
      base::Bind(&SubmitSnapshot, submit_snapshot, data));
  }
}

void ChromeToMobileService::CreateUniqueTempDir() {
  bool success = temp_dir_.CreateUniqueTempDir();
  DCHECK(success);
}

content::URLFetcher* ChromeToMobileService::CreateRequest(
  const RequestData& data) {
  bool get = data.type != SNAPSHOT;
  GURL service_url(cloud_print_url_->GetCloudPrintServiceURL());
  content::URLFetcher* request = content::URLFetcher::Create(
      data.type == SEARCH ? cloud_print::GetUrlForSearch(service_url) :
                            GetSubmitURL(service_url, data),
      get ? content::URLFetcher::GET : content::URLFetcher::POST, this);
  request->SetRequestContext(profile_->GetRequestContext());
  request->SetMaxRetries(kMaxRetries);
  request->SetExtraRequestHeaders("Authorization: OAuth " +
      oauth2_token_ + "\r\n" + cloud_print::kChromeCloudPrintProxyHeader);
  request->SetLoadFlags(net::LOAD_DO_NOT_SEND_COOKIES |
                        net::LOAD_DO_NOT_SAVE_COOKIES);
  return request;
}

void ChromeToMobileService::RequestAuth() {
  DCHECK(oauth2_token_.empty());
  if (oauth2_request_.get())
    return;

  std::string token = TokenServiceFactory::GetForProfile(profile_)->
                          GetOAuth2LoginRefreshToken();
  if (token.empty())
    return;

  oauth2_request_.reset(
      new OAuth2AccessTokenFetcher(this, profile_->GetRequestContext()));
  std::vector<std::string> scopes(1, kOAuth2Scope);
  oauth2_request_->Start(GaiaUrls::GetInstance()->oauth2_chrome_client_id(),
      GaiaUrls::GetInstance()->oauth2_chrome_client_secret(), token, scopes);
}

void ChromeToMobileService::RequestSearch() {
  DCHECK(!oauth2_token_.empty());
  if (search_request_.get())
    return;

  RequestData data;
  data.type = SEARCH;
  search_request_.reset(CreateRequest(data));
  search_request_->Start();
}

void ChromeToMobileService::HandleSearchResponse() {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  std::string data;
  search_request_->GetResponseAsString(&data);
  search_request_.reset();

  DictionaryValue* json_data = NULL;
  cloud_print::ParseResponseJSON(data, NULL, &json_data);

  ListValue* list = NULL;
  if (json_data && json_data->GetList(cloud_print::kPrinterListValue, &list)) {
    std::vector<base::DictionaryValue*> mobiles;
    for (size_t index = 0; index < list->GetSize(); index++) {
      DictionaryValue* mobile_data = NULL;
      if (list->GetDictionary(index, &mobile_data)) {
        std::string mobile_type;
        mobile_data->GetString("type", &mobile_type);
        if (mobile_type.compare(kTypeAndroidChromeSnapshot) == 0)
          mobiles.push_back(mobile_data);
      }
    }
    mobiles_ = mobiles;
  }

  BrowserList::GetLastActiveWithProfile(profile_)->command_updater()->
      UpdateCommandEnabled(IDC_CHROME_TO_MOBILE_PAGE, !mobiles_.empty());

  if (!request_timer_.IsRunning())
    request_timer_.Start(FROM_HERE,
                         base::TimeDelta::FromSeconds(kAutoSearchRetryDelay),
                         this, &ChromeToMobileService::RequestSearch);
}

void ChromeToMobileService::HandleSubmitResponse(
    const content::URLFetcher* source) {
  // Get the observer for this response; bail if there is none or it is NULL.
  RequestObserverMap::iterator i = request_observer_map_.find(source);
  if (i == request_observer_map_.end())
    return;
  base::WeakPtr<Observer> observer = i->second;
  request_observer_map_.erase(i);
  if (!observer.get())
    return;

  // Get the success value from the CloudPrint server response data.
  std::string data;
  source->GetResponseAsString(&data);
  DictionaryValue* json_data = NULL;
  cloud_print::ParseResponseJSON(data, NULL, &json_data);
  bool success = false;
  if (json_data)
    json_data->GetBoolean("success", &success);

  // Check if the observer is waiting on a second response (url and snapshot).
  RequestObserverMap::iterator other = request_observer_map_.begin();
  for (; other != request_observer_map_.end(); ++other) {
    if (other->second == observer) {
      // Do not call OnSendComplete for observers waiting on a second response.
      if (success)
        return;

      // Ensure a second response is not sent after reporting failure below.
      request_observer_map_.erase(other);
      break;
    }
  }

  observer->OnSendComplete(success);
}
