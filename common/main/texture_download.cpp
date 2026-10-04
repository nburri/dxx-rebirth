/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture pack download (texture_download.h): one background thread
 * fetches the manifest and the packs with libcurl, checks them and
 * unpacks them into the user directory.  The main thread only queues
 * jobs and reads the state.
 */

#include "dxxsconf.h"
#include "texture_download.h"
#include "console.h"

#if DXX_USE_CURL
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <curl/curl.h>
#include <physfs.h>
#endif

namespace dcx::texture_download {

#if DXX_USE_CURL
namespace {

namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;

enum class job_kind : std::uint8_t
{
	manifest,
	ensure,
	all,
	remove,
};

struct job
{
	job_kind kind;
	std::string mission;
};

enum class phase : std::uint8_t
{
	idle,
	manifest,
	download,
	install,
};

struct shared_state
{
	std::mutex m;
	std::condition_variable cv;
	std::deque<job> jobs;
	bool busy{};
	bool quit{};
	bool enabled{};
	bool offline{};
	bool curl_ready{};
	std::thread worker;
	fs::path write_dir;
	std::vector<std::string> log;
	/* Worker only (set before it starts, or under m). */
	std::optional<manifest> pack_list;
	clock_type::time_point manifest_tried{};
	bool manifest_failed{};
	std::string status;
	std::string active_mission;
	std::map<std::string, clock_type::time_point> failed;
	std::set<std::string> told;
};

shared_state state;
std::atomic<bool> abort_transfer;
std::atomic<bool> have_log;
std::atomic<phase> current_phase{phase::idle};
std::atomic<std::uint64_t> bytes_done, bytes_total;
std::atomic<unsigned> generation;

void log_line(std::string s)
{
	const std::lock_guard lock{state.m};
	state.log.push_back(std::move(s));
	have_log = true;
}

void set_status(std::string s)
{
	const std::lock_guard lock{state.m};
	state.status = std::move(s);
}

fs::path utf8_path(const std::string_view s)
{
	return fs::path{std::u8string{reinterpret_cast<const char8_t *>(s.data()), s.size()}};
}

/* For messages: UTF-8, also on Windows. */
std::string text(const fs::path &p)
{
	const auto u{p.u8string()};
	return std::string{reinterpret_cast<const char *>(u.data()), u.size()};
}

std::string host_of(const char *const url)
{
	if (!url)
		return {};
	std::string_view u{url};
	if (const auto p{u.find("://")}; p != u.npos)
		u.remove_prefix(p + 3);
	return std::string{u.substr(0, u.find_first_of("/?:"))};
}

std::string megabytes(const std::uint64_t bytes)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024. * 1024.));
	return buf;
}

/* One HTTPS GET; the body goes to sink (false from sink stops the
 * transfer).  Redirects are followed (github.com sends release
 * downloads to release-assets.githubusercontent.com), HTTPS only.
 */
bool http_get(const std::string &url, const std::function<bool(std::span<const std::uint8_t>)> &sink, std::string &error, std::string &final_host)
{
	const std::unique_ptr<CURL, void (*)(CURL *)> curl{curl_easy_init(), curl_easy_cleanup};
	if (!curl)
	{
		error = "libcurl init failed";
		return false;
	}
	CURL *const c{curl.get()};
	std::array<char, CURL_ERROR_SIZE> errbuf{};
	struct context
	{
		const std::function<bool(std::span<const std::uint8_t>)> &sink;
		bool sink_failed{};
	} ctx{sink};
	const curl_write_callback write_cb = [](char *const data, const std::size_t size, const std::size_t n, void *const user) -> std::size_t {
		auto &x{*static_cast<context *>(user)};
		const std::size_t len{size * n};
		if (!x.sink(std::span{reinterpret_cast<const std::uint8_t *>(data), len}))
		{
			x.sink_failed = true;
			return 0;
		}
		return len;
	};
	const curl_xferinfo_callback progress_cb = [](void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
		return abort_transfer ? 1 : 0;
	};
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf.data());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
	const long protocols{CURLPROTO_HTTPS};
	curl_easy_setopt(c, CURLOPT_PROTOCOLS, protocols);
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	/* Give up when less than 1 KB/s arrives for a minute. */
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "dxx-rebirth-texture-download/1");
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
	/* The certificate store of Windows: the packaged game has no CA
	 * bundle file.
	 */
	const long ssl_options{CURLSSLOPT_NATIVE_CA};
	curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, ssl_options);
#elif !defined(_WIN32) && !defined(__APPLE__)
	/* A libcurl bundled with the AppImage looks for the CA file where
	 * the build system had it; other distributions keep it elsewhere.
	 */
	{
		const auto info{curl_version_info(CURLVERSION_NOW)};
		std::error_code ec;
		if (!info || info->age < CURLVERSION_SEVENTH || !info->cainfo || !fs::exists(info->cainfo, ec))
			for (const char *const f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem"})
				if (fs::exists(f, ec))
				{
					curl_easy_setopt(c, CURLOPT_CAINFO, f);
					break;
				}
	}
#endif
	const auto r{curl_easy_perform(c)};
	char *effective{};
	curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
	final_host = host_of(effective);
	if (r == CURLE_OK)
		return true;
	if (ctx.sink_failed && error.empty())
		error = "download larger than announced";
	else if (r == CURLE_ABORTED_BY_CALLBACK)
		error = "cancelled";
	else if (error.empty())
		error = errbuf[0] ? errbuf.data() : curl_easy_strerror(r);
	return false;
}

bool fetch_manifest()
{
	current_phase = phase::manifest;
	state.manifest_tried = clock_type::now();
	std::string body, error, host;
	const bool ok{http_get(std::string{manifest_url}, [&body](const std::span<const std::uint8_t> d) {
		if (body.size() + d.size() > max_manifest_bytes)
			return false;
		body.append(reinterpret_cast<const char *>(d.data()), d.size());
		return true;
	}, error, host)};
	current_phase = phase::idle;
	if (!ok)
	{
		state.manifest_failed = true;
		log_line("manifest: " + error);
		set_status("Offline? " + error.substr(0, 40));
		return false;
	}
	auto m{parse_manifest(body, error)};
	if (!m)
	{
		state.manifest_failed = true;
		log_line(error);
		set_status("Bad pack list from the server");
		return false;
	}
	std::string names;
	for (auto &p : m->packs)
		names += (names.empty() ? "" : ", ") + p.mission + " v" + std::to_string(p.version);
	log_line("manifest from " + host + ": " + std::to_string(m->packs.size()) + " pack(s): " + names);
	set_status(std::to_string(m->packs.size()) + " pack(s) online");
	state.manifest_failed = false;
	state.pack_list = std::move(m);
	return true;
}

/* The manifest of this session: fetched once; after a failure again
 * after a minute.
 */
bool have_manifest()
{
	if (state.pack_list)
		return true;
	if (state.manifest_failed && clock_type::now() - state.manifest_tried < std::chrono::minutes(1))
		return false;
	return fetch_manifest();
}

fs::path textures_dir()
{
	return state.write_dir / "textures";
}

fs::path downloads_dir()
{
	return state.write_dir / "texture-downloads";
}

std::string read_small_file(const fs::path &p)
{
	std::ifstream f{p, std::ios::binary};
	std::string s(256, '\0');
	f.read(s.data(), s.size());
	s.resize(static_cast<std::size_t>(f.gcount()));
	return s;
}

local_state inspect(const std::string &mission, unsigned &version)
{
	std::error_code ec;
	const auto dir{textures_dir() / mission};
	if (!fs::is_directory(dir, ec))
		return local_state::missing;
	if (const auto v{parse_marker(read_small_file(dir / marker_file_name), mission)})
	{
		version = *v;
		return local_state::downloaded;
	}
	return local_state::manual;
}

/* Rename, retried for a while: on Windows a directory cannot be renamed
 * while the game reads a file in it.
 */
bool rename_retry(const fs::path &from, const fs::path &to, std::error_code &ec)
{
	for (unsigned i = 0;; ++i)
	{
		fs::rename(from, to, ec);
		if (!ec || i >= 50)
			return !ec;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

bool unpack(const pack_info &p, const fs::path &zip_path, const fs::path &staging, std::string &error)
{
	std::ifstream zip{zip_path, std::ios::binary};
	if (!zip)
	{
		error = "cannot open the download";
		return false;
	}
	const std::uint64_t file_size{p.size};
	const read_at_function read_at = [&zip](const std::uint64_t offset, const std::span<std::uint8_t> out) {
		zip.clear();
		zip.seekg(static_cast<std::streamoff>(offset));
		zip.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
		return static_cast<std::size_t>(zip.gcount()) == out.size();
	};
	const auto entries{read_zip_directory(file_size, read_at, error)};
	if (!entries)
		return false;
	const auto plan{plan_extraction(*entries, p.mission, p.files, error)};
	if (!plan)
		return false;
	std::error_code ec;
	fs::remove_all(staging, ec);
	fs::create_directories(staging, ec);
	if (ec)
	{
		error = "cannot create " + text(staging) + ": " + ec.message();
		return false;
	}
	std::vector<std::uint8_t> buf(1 << 16);
	for (auto &item : *plan)
	{
		if (abort_transfer)
		{
			error = "cancelled";
			return false;
		}
		const auto &z{(*entries)[item.entry]};
		const auto data{zip_data_offset(z, file_size, read_at, error)};
		if (!data)
			return false;
		std::ofstream out{staging / item.file_name, std::ios::binary | std::ios::trunc};
		std::uint32_t crc{};
		for (std::uint64_t done = 0; done < z.size;)
		{
			/* At most the buffer size. */
			const std::size_t n = std::min<std::uint64_t>(buf.size(), z.size - done);
			const std::span chunk{buf.data(), n};
			if (!read_at(*data + done, chunk))
			{
				error = "read error in " + item.file_name;
				return false;
			}
			if (!done && (n < 8 || std::memcmp(buf.data(), "\x89PNG\r\n\x1a\n", 8)))
			{
				error = item.file_name + " is not a PNG file";
				return false;
			}
			crc = crc32_update(crc, chunk);
			out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(n));
			done += n;
		}
		out.close();
		if (!out)
		{
			error = "cannot write " + item.file_name;
			return false;
		}
		if (crc != z.crc32)
		{
			error = "checksum error in " + item.file_name;
			return false;
		}
	}
	std::ofstream marker{staging / marker_file_name, std::ios::binary | std::ios::trunc};
	marker << format_marker(p);
	marker.close();
	if (!marker)
	{
		error = "cannot write the version file";
		return false;
	}
	return true;
}

bool install(const pack_info &p, std::string &error)
{
	std::error_code ec;
	const auto dl{downloads_dir()};
	fs::create_directories(dl, ec);
	if (ec)
	{
		error = "cannot create " + text(dl) + ": " + ec.message();
		return false;
	}
	const std::string base{p.mission + "-v" + std::to_string(p.version)};
	const auto part{dl / (base + ".zip.part")};
	const auto zip_path{dl / (base + ".zip")};
	const auto staging{dl / (p.mission + ".new")};
	const auto old{dl / (p.mission + ".old")};
	/* Download, counting and hashing as the bytes arrive. */
	{
		current_phase = phase::download;
		bytes_done = 0;
		bytes_total = p.size;
		std::ofstream out{part, std::ios::binary | std::ios::trunc};
		if (!out)
		{
			error = "cannot write " + text(part);
			return false;
		}
		sha256 hash;
		std::uint64_t received{};
		std::string host;
		log_line("downloading " + p.mission + " v" + std::to_string(p.version) + " (" + megabytes(p.size) + ") from " + p.url);
		const auto t0{clock_type::now()};
		const bool ok{http_get(p.url, [&](const std::span<const std::uint8_t> d) {
			if (received + d.size() > p.size)
				return false;
			received += d.size();
			hash.update(d);
			out.write(reinterpret_cast<const char *>(d.data()), static_cast<std::streamsize>(d.size()));
			bytes_done = received;
			return static_cast<bool>(out);
		}, error, host)};
		out.close();
		if (!ok || !out)
		{
			if (ok)
				error = "cannot write " + text(part);
			fs::remove(part, ec);
			return false;
		}
		if (received != p.size)
		{
			error = "size " + std::to_string(received) + " instead of " + std::to_string(p.size);
			fs::remove(part, ec);
			return false;
		}
		if (hash.finish() != p.sha256)
		{
			error = "SHA-256 differs from the manifest";
			fs::remove(part, ec);
			return false;
		}
		const double seconds{std::chrono::duration<double>(clock_type::now() - t0).count()};
		log_line("downloaded " + megabytes(received) + " in " + std::to_string(static_cast<unsigned>(seconds + .5)) + " s from " + host + "; size and SHA-256 match");
		fs::remove(zip_path, ec);
		if (!rename_retry(part, zip_path, ec))
		{
			error = "cannot rename the download: " + ec.message();
			return false;
		}
	}
	current_phase = phase::install;
	const bool unpacked{unpack(p, zip_path, staging, error)};
	fs::remove(zip_path, ec);
	if (!unpacked)
	{
		fs::remove_all(staging, ec);
		return false;
	}
	/* Swap the directories, so that the game sees either the old pack
	 * or the new one.
	 */
	const auto target{textures_dir() / p.mission};
	fs::create_directories(textures_dir(), ec);
	fs::remove_all(old, ec);
	const bool had_old{fs::exists(target, ec)};
	if (had_old && !rename_retry(target, old, ec))
	{
		error = "cannot replace " + text(target) + ": " + ec.message();
		fs::remove_all(staging, ec);
		return false;
	}
	if (!rename_retry(staging, target, ec))
	{
		error = "cannot install " + text(target) + ": " + ec.message();
		if (had_old)
		{
			std::error_code ec2;
			fs::rename(old, target, ec2);
		}
		fs::remove_all(staging, ec);
		return false;
	}
	fs::remove_all(old, ec);
	/* Only if empty. */
	fs::remove(dl, ec);
	++generation;
	return true;
}

void ensure(const std::string &mission, const bool forced)
{
	if (!valid_mission_key(mission) || !have_manifest())
		return;
	const auto p{state.pack_list->find(mission)};
	if (!p)
	{
		if (state.told.insert(mission).second)
			log_line("no pack for mission " + mission);
		return;
	}
	if (!forced)
		if (const auto f{state.failed.find(mission)}; f != state.failed.end() && clock_type::now() - f->second < std::chrono::minutes(10))
			return;
	unsigned version{};
	const auto local{inspect(mission, version)};
	switch (decide(*p, local, version))
	{
		case pack_action::none:
			if (state.told.insert(mission).second)
				log_line(mission + " v" + std::to_string(version) + " is up to date");
			return;
		case pack_action::keep_manual:
			if (state.told.insert(mission).second)
				log_line("textures/" + mission + " has no " + std::string{marker_file_name} + " (installed by hand): not replaced");
			return;
		case pack_action::download:
			break;
	}
	{
		const std::lock_guard lock{state.m};
		state.active_mission = mission;
	}
	if (local == local_state::downloaded)
		log_line(mission + ": v" + std::to_string(version) + " installed, v" + std::to_string(p->version) + " online");
	std::string error;
	const bool ok{install(*p, error)};
	current_phase = phase::idle;
	{
		const std::lock_guard lock{state.m};
		state.active_mission.clear();
	}
	if (ok)
	{
		state.failed.erase(mission);
		state.told.insert(mission);
		log_line(mission + " v" + std::to_string(p->version) + " installed in " + text(textures_dir() / mission) + "; used from the next level load");
		set_status(mission + " v" + std::to_string(p->version) + " ready (next level)");
	}
	else
	{
		/* Not after a cancel: the next level load tries again. */
		if (!abort_transfer)
			state.failed[mission] = clock_type::now();
		log_line(mission + ": " + error + "; the original textures stay");
		set_status(mission + ": " + error.substr(0, 40));
	}
}

void remove_downloaded()
{
	std::error_code ec;
	unsigned removed{};
	for (fs::directory_iterator it{textures_dir(), ec}, end; !ec && it != end; it.increment(ec))
	{
		const auto name{text(it->path().filename())};
		if (!it->is_directory(ec) || !valid_mission_key(name))
			continue;
		if (!parse_marker(read_small_file(it->path() / marker_file_name), name))
			continue;	/* installed by hand */
		std::error_code ec2;
		if (fs::remove_all(it->path(), ec2) != static_cast<std::uintmax_t>(-1) && !ec2)
		{
			++removed;
			log_line("deleted " + text(it->path()));
		}
		else
			log_line("cannot delete " + text(it->path()) + ": " + ec2.message());
	}
	fs::remove_all(downloads_dir(), ec);
	state.told.clear();
	state.failed.clear();
	++generation;
	set_status(std::to_string(removed) + " downloaded pack(s) deleted");
}

void worker_main()
{
	for (;;)
	{
		job j;
		{
			std::unique_lock lock{state.m};
			state.busy = false;
			state.cv.notify_all();
			state.cv.wait(lock, [] { return state.quit || !state.jobs.empty(); });
			if (state.quit)
				return;
			j = std::move(state.jobs.front());
			state.jobs.pop_front();
			state.busy = true;
			/* Under the lock: a cancel (set_enabled, shutdown,
			 * delete_downloaded) after this point stays set.
			 */
			abort_transfer = false;
		}
		try {
			switch (j.kind)
			{
				case job_kind::manifest:
					have_manifest();
					break;
				case job_kind::ensure:
					ensure(j.mission, false);
					break;
				case job_kind::all:
					/* A fresh list: the user asked for it. */
					state.pack_list.reset();
					state.manifest_failed = false;
					if (have_manifest())
					{
						std::vector<std::string> missions;
						for (auto &p : state.pack_list->packs)
							missions.push_back(p.mission);
						for (auto &m : missions)
							if (!abort_transfer)
								ensure(m, true);
					}
					break;
				case job_kind::remove:
					remove_downloaded();
					break;
			}
		} catch (const std::exception &e) {
			log_line(std::string{"error: "} + e.what());
		}
		current_phase = phase::idle;
	}
}

void enqueue(job j)
{
	{
		const std::lock_guard lock{state.m};
		if (!state.enabled || state.offline || !state.curl_ready || state.quit)
			return;
		for (auto &q : state.jobs)
			if (q.kind == j.kind && q.mission == j.mission)
				return;
		state.jobs.push_back(std::move(j));
		if (!state.worker.joinable())
			state.worker = std::thread{worker_main};
	}
	state.cv.notify_all();
}

struct join_at_exit
{
	~join_at_exit()
	{
		shutdown();
	}
} join_at_exit_instance;

}

bool available()
{
	return true;
}

void start(const bool enabled, const bool offline)
{
	{
		const std::lock_guard lock{state.m};
		state.offline = offline;
		if (const auto w{PHYSFS_getWriteDir()})
			state.write_dir = utf8_path(w);
		if (!offline && !state.write_dir.empty() && !state.curl_ready)
		{
#if defined(_WIN32)
			/* Schannel uses the certificate store of Windows. */
			curl_global_sslset(CURLSSLBACKEND_SCHANNEL, nullptr, nullptr);
#endif
			state.curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
		}
	}
	if (offline)
		log_line("off (-notexturedownload)");
	set_enabled(enabled);
}

void set_enabled(const bool enabled)
{
	{
		const std::lock_guard lock{state.m};
		state.enabled = enabled;
		if (!enabled)
		{
			state.jobs.clear();
			abort_transfer = true;
		}
	}
	if (enabled)
		enqueue({job_kind::manifest, {}});
}

void level_loaded(const std::string_view mission)
{
	if (valid_mission_key(mission))
		enqueue({job_kind::ensure, std::string{mission}});
}

void download_all()
{
	enqueue({job_kind::all, {}});
}

void delete_downloaded()
{
	/* Allowed also when downloads are off. */
	{
		const std::lock_guard lock{state.m};
		if (state.write_dir.empty() || state.quit)
			return;
		state.jobs.clear();
		abort_transfer = true;
		state.jobs.push_back({job_kind::remove, {}});
		if (!state.worker.joinable())
			state.worker = std::thread{worker_main};
	}
	state.cv.notify_all();
}

void wait_idle(const unsigned timeout_ms)
{
	const auto end{clock_type::now() + std::chrono::milliseconds(timeout_ms)};
	for (;;)
	{
		poll();
		{
			std::unique_lock lock{state.m};
			if (state.jobs.empty() && !state.busy)
				break;
			if (state.cv.wait_until(lock, std::min(end, clock_type::now() + std::chrono::milliseconds(200))) == std::cv_status::timeout && clock_type::now() >= end)
				break;
		}
	}
	poll();
}

unsigned installed_generation()
{
	return generation;
}

std::string status_text()
{
	if (state.offline)
		return "Download off (-notexturedownload)";
	switch (current_phase.load())
	{
		case phase::manifest:
			return "Checking for packs...";
		case phase::download:
		{
			const std::lock_guard lock{state.m};
			return "Downloading " + state.active_mission + ": " + std::to_string(bytes_done / (1024 * 1024)) + " of " + std::to_string(bytes_total / (1024 * 1024)) + " MB";
		}
		case phase::install:
		{
			const std::lock_guard lock{state.m};
			return "Installing " + state.active_mission + "...";
		}
		case phase::idle:
		default:
			break;
	}
	const std::lock_guard lock{state.m};
	if (!state.enabled)
		return {};
	return state.status;
}

std::string hud_text()
{
	const auto p{current_phase.load()};
	if (p != phase::download && p != phase::install)
		return {};
	const std::lock_guard lock{state.m};
	if (p == phase::install)
		return "HD textures " + state.active_mission + ": installing";
	const std::uint64_t total{bytes_total};
	return "HD textures " + state.active_mission + ": " + std::to_string(total ? bytes_done * 100 / total : 0) + "%";
}

void poll()
{
	if (!have_log.exchange(false))
		return;
	std::vector<std::string> lines;
	{
		const std::lock_guard lock{state.m};
		lines.swap(state.log);
	}
	for (auto &l : lines)
		con_printf(CON_NORMAL, "Texture download: %s", l.c_str());
}

void shutdown()
{
	std::thread t;
	{
		const std::lock_guard lock{state.m};
		state.quit = true;
		state.jobs.clear();
		abort_transfer = true;
		t = std::move(state.worker);
	}
	state.cv.notify_all();
	if (t.joinable())
		t.join();
}

#else

bool available()
{
	return false;
}

void start(bool, bool)
{
}

void set_enabled(bool)
{
}

void level_loaded(std::string_view)
{
}

void download_all()
{
}

void delete_downloaded()
{
}

void wait_idle(unsigned)
{
}

unsigned installed_generation()
{
	return 0;
}

std::string status_text()
{
	return "Download not in this build (no libcurl)";
}

std::string hud_text()
{
	return {};
}

void poll()
{
}

void shutdown()
{
}

#endif

}
