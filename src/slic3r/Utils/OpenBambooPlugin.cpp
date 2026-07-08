#include "OpenBambooPlugin.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace OpenBambooPlugin {

namespace {

// ── Platform description ─────────────────────────────────────────────────────

struct PlatformInfo
{
    std::string asset;         // GitHub release asset file name
    bool        is_zip;        // .zip (miniz) vs .tar.gz (gzip+tar)
    std::string src_plugin;    // plugin binary name inside the archive
    std::string src_source;    // BambuSource binary name inside the archive
    std::string src_live555;   // live555 binary name inside the archive
    std::string dst_source;    // installed BambuSource name
    std::string dst_live555;   // installed live555 name
    std::string ext;           // installed plugin extension (.so/.dylib/.dll)
    bool        lib_prefix;    // whether the installed plugin uses the "lib" prefix
};

PlatformInfo platform_info()
{
    PlatformInfo p;
#if defined(_WIN32)
    // Only an x64 Windows build is published; use it for arm64 too.
    p.asset       = "obn-windows-x64.zip";
    p.is_zip      = true;
    p.src_plugin  = "bambu_networking.dll";
    p.src_source  = "BambuSource.dll";
    p.src_live555 = "live555.dll";
    p.dst_source  = "BambuSource.dll";
    p.dst_live555 = "live555.dll";
    p.ext         = ".dll";
    p.lib_prefix  = false;
#elif defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    p.asset = "obn-macos-arm64.tar.gz";
  #else
    p.asset = "obn-macos-x64.tar.gz";
  #endif
    p.is_zip      = false;
    p.src_plugin  = "libbambu_networking.dylib";
    p.src_source  = "libBambuSource.dylib";
    p.src_live555 = "liblive555.dylib";
    p.dst_source  = "libBambuSource.dylib";
    p.dst_live555 = "liblive555.dylib";
    p.ext         = ".dylib";
    p.lib_prefix  = true;
#else // Linux
  #if defined(__aarch64__) || defined(__arm64__)
    p.asset = "obn-linux-aarch64.tar.gz";
  #else
    p.asset = "obn-linux-x64.tar.gz";
  #endif
    p.is_zip      = false;
    p.src_plugin  = "libbambu_networking.so";
    p.src_source  = "libBambuSource.so";
    p.src_live555 = "liblive555.so";
    p.dst_source  = "libBambuSource.so";
    p.dst_live555 = "liblive555.so";
    p.ext         = ".so";
    p.lib_prefix  = true;
#endif
    return p;
}

// Installed plugin file name Orca loads for a given version, e.g.
// "libbambu_networking_02.03.00.99.so" or "bambu_networking_02.03.00.99.dll".
std::string installed_plugin_name(const PlatformInfo& p, const std::string& version)
{
    std::string name = p.lib_prefix ? "lib" : "";
    name += std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + p.ext;
    return name;
}

// major.minor.patch of the network plugin version this Orca build expects,
// e.g. "02.03.00.62" -> "02.03.00". Used to pick the matching OBN ABI dir.
std::string target_abi_prefix()
{
    std::string full = get_latest_network_version();
    size_t      pos  = 0;
    int         dots = 0;
    for (; pos < full.size(); ++pos) {
        if (full[pos] == '.' && ++dots == 3)
            break;
    }
    return full.substr(0, pos);
}

// ── gzip + tar (POSIX archives), no external deps beyond miniz's tinfl ────────

std::vector<unsigned char> gunzip(const std::string& in)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    const size_t         n = in.size();
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 0x08)
        throw std::runtime_error("Downloaded file is not a valid gzip archive.");

    const unsigned char flg = p[3];
    size_t              off = 10;
    if (flg & 0x04) { // FEXTRA
        if (off + 2 > n) throw std::runtime_error("Corrupt gzip header.");
        size_t xlen = p[off] | (p[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) { while (off < n && p[off]) ++off; ++off; } // FNAME
    if (flg & 0x10) { while (off < n && p[off]) ++off; ++off; } // FCOMMENT
    if (flg & 0x02) off += 2;                                   // FHCRC
    if (off + 8 > n) throw std::runtime_error("Corrupt gzip stream.");

    const unsigned char* deflate     = p + off;
    const size_t         deflate_len = n - off - 8; // trailer = crc32(4) + isize(4)

    size_t out_len = 0;
    void*  out     = tinfl_decompress_mem_to_heap(deflate, deflate_len, &out_len, 0 /*raw deflate*/);
    if (!out)
        throw std::runtime_error("Failed to decompress the downloaded archive.");
    std::vector<unsigned char> result(reinterpret_cast<unsigned char*>(out),
                                      reinterpret_cast<unsigned char*>(out) + out_len);
    mz_free(out);
    return result;
}

size_t parse_octal(const char* s, size_t len)
{
    size_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c < '0' || c > '7')
            continue;
        v = v * 8 + static_cast<size_t>(c - '0');
    }
    return v;
}

// Extract a ustar tar image into dest. Only regular files are written; long-name
// (GNU 'L') entries are not needed — OBN paths fit the 100-byte name field.
void untar(const std::vector<unsigned char>& data, const fs::path& dest)
{
    size_t off = 0;
    while (off + 512 <= data.size()) {
        const char* h = reinterpret_cast<const char*>(&data[off]);

        bool all_zero = true;
        for (int i = 0; i < 512; ++i) {
            if (h[i]) { all_zero = false; break; }
        }
        if (all_zero)
            break; // end-of-archive marker

        char name[101];   std::memcpy(name, h, 100);        name[100] = 0;
        char prefix[156]; std::memcpy(prefix, h + 345, 155); prefix[155] = 0;
        const size_t size     = parse_octal(h + 124, 12);
        const char   typeflag = h[156];

        std::string full = prefix[0] ? (std::string(prefix) + "/" + name) : std::string(name);
        off += 512;

        if ((typeflag == '0' || typeflag == '\0') && !full.empty() && full.back() != '/') {
            fs::path outp = dest / full;
            boost::system::error_code ec;
            fs::create_directories(outp.parent_path(), ec);
            std::ofstream ofs(outp.string(), std::ios::binary | std::ios::trunc);
            if (size > 0 && off + size <= data.size())
                ofs.write(reinterpret_cast<const char*>(&data[off]), size);
        }
        off += ((size + 511) / 512) * 512;
    }
}

void unzip_all(const std::string& zip_path, const fs::path& dest)
{
    mz_zip_archive ar;
    mz_zip_zero_struct(&ar);
    if (!open_zip_reader(&ar, zip_path))
        throw std::runtime_error("Failed to open the downloaded zip archive.");

    mz_uint num = mz_zip_reader_get_num_files(&ar);
    for (mz_uint i = 0; i < num; ++i) {
        if (mz_zip_reader_is_file_a_directory(&ar, i))
            continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&ar, i, &st))
            continue;
        fs::path outp = dest / st.m_filename;
        boost::system::error_code ec;
        fs::create_directories(outp.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&ar, i, outp.string().c_str(), 0)) {
            close_zip_reader(&ar);
            throw std::runtime_error(std::string("Failed to extract ") + st.m_filename);
        }
    }
    close_zip_reader(&ar);
}

// Locate the "lib" directory holding the v<ABI> ABI folders inside the extracted tree.
fs::path find_lib_dir(const fs::path& root)
{
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (fs::is_directory(it->status()) && it->path().filename() == "lib")
            return it->path();
    }
    return {};
}

// Pick lib/v<target> if present, else the highest-versioned v* directory.
fs::path pick_abi_dir(const fs::path& lib_dir, const std::string& target_abi, std::string& out_abi)
{
    fs::path    best;
    std::string best_ver;
    boost::system::error_code ec;
    for (fs::directory_iterator it(lib_dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!fs::is_directory(it->status()))
            continue;
        std::string fn = it->path().filename().string();
        if (fn.size() < 2 || fn[0] != 'v')
            continue;
        std::string ver = fn.substr(1); // strip leading 'v'
        if (ver == target_abi) {
            out_abi = ver;
            return it->path(); // exact match wins
        }
        if (best_ver.empty() || ver > best_ver) {
            best_ver = ver;
            best     = it->path();
        }
    }
    out_abi = best_ver;
    return best;
}

void copy_over(const fs::path& src, const fs::path& dst)
{
    boost::system::error_code ec;
    if (fs::exists(dst))
        fs::remove(dst, ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("Failed to install " + dst.filename().string() + ": " + ec.message());
}

bool check(const ProgressFn& progress, int percent, const std::string& stage)
{
    if (progress && !progress(percent, stage))
        throw std::runtime_error("Canceled.");
    return true;
}

} // namespace

std::string install_latest(const ProgressFn& progress)
{
    const PlatformInfo plat = platform_info();

    // 1) Query the latest GitHub release for the platform asset URL.
    check(progress, 2, "Querying GitHub for the latest release");
    const std::string api_url = std::string("https://api.github.com/repos/") + OBN_REPO_OWNER + "/" +
                                OBN_REPO_NAME + "/releases/latest";

    std::string download_url;
    std::string tag;
    std::string api_err;
    Http::get(api_url)
        .header("Accept", "application/vnd.github+json")
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string body, unsigned /*status*/) {
            try {
                auto j = nlohmann::json::parse(body);
                tag    = j.value("tag_name", "");
                for (const auto& a : j.at("assets")) {
                    if (a.value("name", "") == plat.asset) {
                        download_url = a.value("browser_download_url", "");
                        break;
                    }
                }
            } catch (const std::exception& e) {
                api_err = std::string("Could not parse GitHub response: ") + e.what();
            }
        })
        .on_error([&](std::string /*body*/, std::string error, unsigned status) {
            api_err = "GitHub request failed (" + std::to_string(status) + "): " + error;
        })
        .perform_sync();

    if (!api_err.empty())
        throw std::runtime_error(api_err);
    if (download_url.empty())
        throw std::runtime_error("No Open Bamboo Networking asset '" + plat.asset + "' in the latest release.");
    BOOST_LOG_TRIVIAL(info) << "[OBN] latest release " << tag << ", asset url = " << download_url;

    // 2) Download the archive to a temp file.
    check(progress, 5, "Downloading Open Bamboo Networking " + tag);
    fs::path archive_path = fs::temp_directory_path() /
                            (std::string("obn_") + std::to_string(get_current_pid()) + "_" + plat.asset);

    std::string body;
    std::string dl_err;
    Http::get(download_url)
        .timeout_connect(10)
        .timeout_max(300)
        .on_progress([&](Http::Progress p, bool& cancel) {
            if (progress && p.dltotal > 0) {
                int pct = 5 + static_cast<int>(p.dlnow * 65 / p.dltotal); // 5..70
                if (!progress(pct, "Downloading Open Bamboo Networking " + tag))
                    cancel = true;
            }
        })
        .on_complete([&](std::string b, unsigned /*status*/) { body = std::move(b); })
        .on_error([&](std::string /*b*/, std::string error, unsigned status) {
            dl_err = "Download failed (" + std::to_string(status) + "): " + error;
        })
        .perform_sync();

    if (!dl_err.empty())
        throw std::runtime_error(dl_err);
    if (body.empty())
        throw std::runtime_error("Downloaded archive is empty.");

    {
        std::ofstream ofs(archive_path.string(), std::ios::binary | std::ios::trunc);
        ofs.write(body.data(), body.size());
    }

    // 3) Extract into a temp directory.
    check(progress, 72, "Extracting");
    fs::path extract_dir = fs::temp_directory_path() /
                           (std::string("obn_extract_") + std::to_string(get_current_pid()));
    boost::system::error_code ec;
    fs::remove_all(extract_dir, ec);
    fs::create_directories(extract_dir, ec);

    struct Cleanup {
        fs::path a, d;
        ~Cleanup() { boost::system::error_code e; fs::remove(a, e); fs::remove_all(d, e); }
    } cleanup{archive_path, extract_dir};

    if (plat.is_zip)
        unzip_all(archive_path.string(), extract_dir);
    else
        untar(gunzip(body), extract_dir);

    // 4) Select the ABI directory matching this Orca build.
    check(progress, 82, "Selecting plugin version");
    fs::path lib_dir = find_lib_dir(extract_dir);
    if (lib_dir.empty())
        throw std::runtime_error("Unexpected archive layout: no lib/ directory found.");

    const std::string target = target_abi_prefix();
    std::string       abi;
    fs::path          abi_dir = pick_abi_dir(lib_dir, target, abi);
    if (abi_dir.empty() || abi.empty())
        throw std::runtime_error("No compatible plugin ABI found in the archive.");
    if (abi != target)
        BOOST_LOG_TRIVIAL(warning) << "[OBN] exact ABI " << target << " not found; using " << abi;

    const std::string version = abi + ".99"; // OBN marks its builds with the .99 patch suffix

    // 5) Install into data_dir()/plugins.
    check(progress, 88, "Installing");
    fs::path plugins = fs::path(data_dir()) / "plugins";
    fs::create_directories(plugins, ec);

    fs::path src_plugin = abi_dir / plat.src_plugin;
    if (!fs::exists(src_plugin))
        throw std::runtime_error("Plugin binary missing from the selected ABI directory.");
    copy_over(src_plugin, plugins / installed_plugin_name(plat, version));

    fs::path src_source = abi_dir / plat.src_source;
    if (fs::exists(src_source))
        copy_over(src_source, plugins / plat.dst_source);

    // live555: keep an existing large vendor build if present (matches OBN install.sh).
    fs::path src_live555 = abi_dir / plat.src_live555;
    fs::path dst_live555 = plugins / plat.dst_live555;
    if (fs::exists(src_live555)) {
        bool install_live555 = true;
        if (fs::exists(dst_live555) && fs::file_size(dst_live555) > 65536)
            install_live555 = false;
        if (install_live555)
            copy_over(src_live555, dst_live555);
    }

    check(progress, 100, "Installed");
    BOOST_LOG_TRIVIAL(info) << "[OBN] installed version " << version << " into " << plugins.string();
    return version;
}

bool seed_lan_conf()
{
    fs::path conf = fs::path(data_dir()) / "obn.conf";
    if (fs::exists(conf))
        return false; // never clobber user settings

    boost::system::error_code ec;
    std::ofstream ofs(conf.string(), std::ios::trunc);
    if (!ofs)
        return false;
    ofs << "# Open Bamboo Networking configuration (created by OrcaSlicer)\n"
        << "# LAN-only: never open background cloud connections.\n"
        << "block_cloud = 1\n"
        << "log_to_file = 0\n";
    BOOST_LOG_TRIVIAL(info) << "[OBN] seeded LAN-only obn.conf at " << conf.string();
    return true;
}

}} // namespace Slic3r::OpenBambooPlugin
