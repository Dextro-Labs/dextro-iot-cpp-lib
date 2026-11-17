#include <dextro-iot/ota/curl_downloader.hpp>

#include <dextro-iot/ota/fs.hpp>

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>

namespace dextro::iot::ota {
namespace {

class Transfer {
public:
    std::FILE* file = nullptr;
    CURL* curl = nullptr;
    std::int64_t resumed_from = 0;
    std::int64_t written = 0;  ///< neste arquivo, contando o que já havia
    std::int64_t expected = 0;
    bool restarted = false;
    bool too_big = false;
    bool write_error = false;
    const KeepGoing* keep_going = nullptr;
    bool stopped = false;
};

std::size_t on_data(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* t = static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    long code = 0;
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200 && code != 206) {
        return n;  // corpo de erro: não vai para o .part que será retomado
    }
    if (!t->restarted && t->resumed_from > 0) {
        // Pediu Range e o servidor mandou o arquivo inteiro (200): recomeça.
        if (code == 200) {
            if (std::fflush(t->file) != 0 || ::ftruncate(fileno(t->file), 0) != 0 ||
                std::fseek(t->file, 0, SEEK_SET) != 0) {
                t->write_error = true;
                return 0;
            }
            t->written = 0;
        }
        t->restarted = true;
    }
    if (t->expected > 0 && t->written + static_cast<std::int64_t>(n) > t->expected) {
        t->too_big = true;
        return 0;
    }
    if (std::fwrite(data, 1, n, t->file) != n) {
        t->write_error = true;
        return 0;
    }
    t->written += static_cast<std::int64_t>(n);
    return n;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* t = static_cast<Transfer*>(user);
    if (t->keep_going != nullptr && *t->keep_going && !(*t->keep_going)()) {
        t->stopped = true;
        return 1;
    }
    return 0;
}

void global_init()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace

CurlDownloader::CurlDownloader() : CurlDownloader(Options{})
{
}

CurlDownloader::CurlDownloader(Options options) : options_(std::move(options))
{
}

DownloadResult CurlDownloader::fetch(const DownloadRequest& request, const KeepGoing& keep_going)
{
    global_init();
    DownloadResult r;

    Transfer t;
    t.resumed_from = std::max<std::int64_t>(fs::file_size(request.path), 0);
    t.written = t.resumed_from;
    t.expected = request.expected_size;
    t.keep_going = &keep_going;
    if (t.expected > 0 && t.resumed_from == t.expected) {
        // Já está inteiro (caiu entre o fim do download e o rename).
        r.outcome = DownloadResult::Outcome::Ok;
        r.total_bytes = t.resumed_from;
        return r;
    }
    if (t.expected > 0 && t.resumed_from > t.expected) {
        fs::remove_file(request.path);
        t.resumed_from = 0;
        t.written = 0;
    }
    if (request.url.rfind("https://", 0) != 0) {
        r.outcome = DownloadResult::Outcome::Permanent;
        r.code = "DOWNLOAD_FAILED";
        r.message = "artifactUrl não é https://";
        r.retryable = false;
        return r;
    }

    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(request.path.c_str(), "abe"),
                                                         &std::fclose);
    if (!file) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "STORAGE";
        r.message = "não abriu " + request.path;
        return r;
    }
    t.file = file.get();

    std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "DOWNLOAD_FAILED";
        r.message = "curl_easy_init";
        return r;
    }
    t.curl = curl.get();

    // O token vai só neste header e só para este host: sem seguir
    // redirecionamento, ele não vaza para outro servidor.
    const std::string auth = "device-auth: " + request.token;
    std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(
        curl_slist_append(nullptr, auth.c_str()), &curl_slist_free_all);

    CURL* c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0: CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!options_.ca_file.empty()) {
        curl_easy_setopt(c, CURLOPT_CAINFO, options_.ca_file.c_str());
    }
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, options_.connect_timeout_s);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, options_.low_speed_limit);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, options_.low_speed_time_s);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, options_.user_agent.c_str());
    if (t.resumed_from > 0) {
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(t.resumed_from));
    }

    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    r.http_status = status;
    const bool flushed = std::fflush(file.get()) == 0 && ::fsync(fileno(file.get())) == 0;
    file.reset();
    r.total_bytes = std::max<std::int64_t>(fs::file_size(request.path), 0);

    if (t.stopped) {
        r.outcome = DownloadResult::Outcome::Stopped;
        return r;
    }
    if (t.too_big) {
        fs::remove_file(request.path);
        r.outcome = DownloadResult::Outcome::Permanent;
        r.code = "SIZE_MISMATCH";
        r.message = "o servidor mandou mais que os " + std::to_string(t.expected) +
                    " bytes do manifesto";
        r.retryable = true;
        return r;
    }
    if (t.write_error || !flushed) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "STORAGE";
        r.message = "falha de escrita em " + request.path;
        return r;
    }
    if (status == 401 || status == 403) {
        fs::remove_file(request.path);
        r.outcome = DownloadResult::Outcome::Permanent;
        r.code = "AUTH_REJECTED";
        r.message = "o artifact-download recusou o device-auth (HTTP " + std::to_string(status) + ")";
        r.retryable = false;
        return r;
    }
    if (status == 404) {
        fs::remove_file(request.path);
        r.outcome = DownloadResult::Outcome::Permanent;
        r.code = "ARTIFACT_NOT_FOUND";
        r.message = "HTTP 404 em " + request.url;
        r.retryable = false;
        return r;
    }
    if (status == 416) {
        // O que havia no .part não casa com o arquivo: recomeça do zero.
        fs::remove_file(request.path);
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "DOWNLOAD_FAILED";
        r.message = "HTTP 416 ao retomar; recomeçando";
        r.total_bytes = 0;
        return r;
    }
    if (rc != CURLE_OK) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "DOWNLOAD_FAILED";
        r.message = std::string("curl: ") + curl_easy_strerror(rc);
        return r;
    }
    if (status != 200 && status != 206) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "DOWNLOAD_FAILED";
        r.message = "HTTP " + std::to_string(status);
        return r;
    }
    if (t.expected > 0 && r.total_bytes != t.expected) {
        r.outcome = DownloadResult::Outcome::Transient;
        r.code = "DOWNLOAD_FAILED";
        r.message = "a conexão fechou em " + std::to_string(r.total_bytes) + " de " +
                    std::to_string(t.expected) + " bytes";
        return r;
    }
    r.outcome = DownloadResult::Outcome::Ok;
    return r;
}

}  // namespace dextro::iot::ota
