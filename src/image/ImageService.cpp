#include "image/ImageService.h"

#include "image/Loader.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <string_view>
#include <unordered_set>

namespace rv
{

ImageService::ImageService(std::function<void()> notify,
                           size_t budgetBytes,
                           int threads)
    : m_notify(std::move(notify)), m_budget(budgetBytes)
{
	for(int i = 0; i < threads; ++i)
	{
		m_threads.emplace_back([this] { run(); });
	}
}

ImageService::~ImageService()
{
	{
		std::lock_guard lk(m_mu);
		m_stop = true;
		m_queue.clear();
	}
	m_cv.notify_all();
	for(auto& t : m_threads)
	{
		t.join();
	}
}

namespace
{
std::string fileStamp(const std::filesystem::path& p)
{
	std::error_code ec;
	auto mtime = std::filesystem::last_write_time(p, ec);
	if(ec)
	{
		return "missing";
	}
	auto size = std::filesystem::file_size(p, ec);
	return fmt::format("{}:{}",
	                   mtime.time_since_epoch().count(),
	                   ec ? 0 : size);
}
} // namespace

std::string ImageService::identLocked(const std::filesystem::path& p)
{
	std::string s = p.string();
	auto it = m_stamps.find(s);
	if(it == m_stamps.end())
	{
		it = m_stamps.emplace(s, fileStamp(p)).first;
	}
	return s + "|" + it->second;
}

std::string ImageService::layerKey(const std::string& ident,
                                   const std::string& layer,
                                   int reduce)
{
	return fmt::format("L|{}|{}|{}", ident, layer, reduce);
}

bool ImageService::checkForChanges(
    const std::vector<std::filesystem::path>& paths)
{
	// Stat outside the lock: slow filesystems must not block the workers.
	std::vector<std::pair<std::string, std::string>> now;
	now.reserve(paths.size());
	for(const auto& p : paths)
	{
		now.emplace_back(p.string(), fileStamp(p));
	}
	bool changed = false;
	std::lock_guard lk(m_mu);
	for(auto& [path, stamp] : now)
	{
		auto it = m_stamps.find(path);
		if(it == m_stamps.end() || it->second == stamp)
		{
			continue; // never requested, or unchanged
		}
		spdlog::info("changed on disk: {}", path);
		std::string old = path + "|" + it->second;
		it->second = stamp;
		// Its diffs too: "D|identA|identB…" holds it either side.
		auto inDiff = [&](const std::string& k)
		{
			return k.starts_with("D|") &&
			       k.find(old + "|") != std::string::npos;
		};
		std::erase_if(m_errors,
		              [&](const auto& e)
		              { return e.first == old || inDiff(e.first); });
		for(auto c = m_cache.begin(); c != m_cache.end();)
		{
			const std::string& k = c->first;
			bool stale = k.compare(0, old.size() + 2, "I|" + old) == 0 ||
			             k.compare(0, old.size() + 3, "L|" + old + "|") == 0 ||
			             inDiff(k);
			if(stale)
			{
				m_bytes -= c->second.m_bytes;
				m_lru.erase(c->second.m_lru);
				c = m_cache.erase(c);
			}
			else
			{
				++c;
			}
		}
		changed = true;
	}
	return changed;
}

template <class T>
std::shared_ptr<const T> ImageService::get(const std::string& key)
{
	auto it = m_cache.find(key);
	if(it == m_cache.end())
	{
		return nullptr;
	}
	m_lru.splice(m_lru.begin(), m_lru, it->second.m_lru);
	if(auto* v = std::get_if<std::shared_ptr<const T>>(&it->second.m_value))
	{
		return *v;
	}
	return nullptr;
}

ImageInfoPtr ImageService::info(const std::filesystem::path& p, Priority pr)
{
	std::string ident, key;
	{
		std::lock_guard lk(m_mu);
		ident = identLocked(p);
		key = "I|" + ident;
		if(auto v = get<ImageInfo>(key))
		{
			return v;
		}
		if(m_errors.count(ident))
		{
			return nullptr;
		}
	}
	enqueue({key, ident, p, std::nullopt, 1, pr});
	return nullptr;
}

LayerImagePtr ImageService::layer(const std::filesystem::path& p,
                                  const std::string& label,
                                  int reduce,
                                  Priority pr)
{
	reduce = std::max(1, reduce);
	std::string ident, key;
	{
		std::lock_guard lk(m_mu);
		ident = identLocked(p);
		key = layerKey(ident, label, reduce);
		if(auto v = get<LayerImage>(key))
		{
			return v;
		}
		if(m_errors.count(ident))
		{
			return nullptr;
		}
	}
	enqueue({key, ident, p, label, reduce, pr});
	return nullptr;
}

LayerImagePtr ImageService::layerBestEffort(const std::filesystem::path& p,
                                            const std::string& label,
                                            int reduce,
                                            Priority pr)
{
	if(auto v = layer(p, label, reduce, pr))
	{
		return v;
	}
	std::lock_guard lk(m_mu);
	std::string ident = identLocked(p);
	// Prefer finer resolutions first.
	for(int r = 1; r <= 64; ++r)
	{
		if(r != reduce)
		{
			if(auto v = get<LayerImage>(layerKey(ident, label, r)))
			{
				return v;
			}
		}
	}
	return nullptr;
}

std::string ImageService::diffIdentLocked(const std::filesystem::path& a,
                                          const std::filesystem::path& b)
{
	return "D|" + identLocked(a) + "|" + identLocked(b);
}

LayerImagePtr ImageService::diffLayer(const std::filesystem::path& a,
                                      const std::filesystem::path& b,
                                      const std::string& label,
                                      int reduce,
                                      Priority pr)
{
	reduce = std::max(1, reduce);
	Job job{.m_path = b, .m_layer = label, .m_reduce = reduce, .m_pr = pr};
	{
		std::lock_guard lk(m_mu);
		job.m_ident = diffIdentLocked(a, b);
		job.m_key = fmt::format("{}|{}|{}", job.m_ident, label, reduce);
		if(auto v = get<LayerImage>(job.m_key))
		{
			return v;
		}
		if(m_errors.count(job.m_ident))
		{
			return nullptr;
		}
		job.m_identA = identLocked(a);
		job.m_identB = identLocked(b);
	}
	job.m_diffA = a;
	enqueue(std::move(job));
	return nullptr;
}

std::optional<std::string> ImageService::diffError(
    const std::filesystem::path& a, const std::filesystem::path& b)
{
	std::lock_guard lk(m_mu);
	auto it = m_errors.find(diffIdentLocked(a, b));
	if(it == m_errors.end())
	{
		return std::nullopt;
	}
	return it->second;
}

bool ImageService::hasLayer(const std::filesystem::path& p,
                            const std::string& label,
                            int reduce)
{
	std::lock_guard lk(m_mu);
	return m_cache.count(layerKey(identLocked(p), label, std::max(1, reduce))) >
	       0;
}

std::vector<bool>
ImageService::cachedLayers(const std::vector<std::filesystem::path>& paths,
                           const std::string& layerLabel)
{
	std::lock_guard lk(m_mu);
	// Keys are "L|<ident>|<label>|<reduce>": the idents with the label
	// decoded at some resolution.
	const std::string tail = "|" + layerLabel + "|";
	std::unordered_set<std::string_view> decoded;
	for(const auto& [key, entry] : m_cache)
	{
		const auto at = key.rfind(tail);
		if(key.starts_with("L|") && at != std::string::npos && at > 2)
		{
			decoded.insert(std::string_view(key).substr(2, at - 2));
		}
	}
	std::vector<bool> out(paths.size(), false);
	for(size_t i = 0; i < paths.size() && !decoded.empty(); ++i)
	{
		const std::string path = paths[i].string();
		auto it = m_stamps.find(path);
		out[i] =
		    it != m_stamps.end() && decoded.contains(path + "|" + it->second);
	}
	return out;
}

void ImageService::demote(const std::filesystem::path& p,
                          const std::string& label,
                          int reduce)
{
	std::lock_guard lk(m_mu);
	auto it =
	    m_cache.find(layerKey(identLocked(p), label, std::max(1, reduce)));
	if(it != m_cache.end())
	{
		m_lru.splice(m_lru.end(),
		             m_lru,
		             it->second.m_lru); // back: evicted first
	}
}

std::optional<std::string> ImageService::error(const std::filesystem::path& p)
{
	std::lock_guard lk(m_mu);
	auto it = m_errors.find(identLocked(p));
	if(it == m_errors.end())
	{
		return std::nullopt;
	}
	return it->second;
}

void ImageService::clearPrefetch()
{
	std::lock_guard lk(m_mu);
	for(auto it = m_queue.begin(); it != m_queue.end();)
	{
		if(it->m_pr == Priority::PREFETCH)
		{
			m_inFlight.erase(it->m_key);
			it = m_queue.erase(it);
		}
		else
		{
			++it;
		}
	}
}

size_t ImageService::bytesUsed()
{
	std::lock_guard lk(m_mu);
	return m_bytes;
}

size_t ImageService::pendingJobs()
{
	std::lock_guard lk(m_mu);
	return m_inFlight.size();
}

void ImageService::enqueue(Job job)
{
	{
		std::lock_guard lk(m_mu);
		auto it = m_inFlight.find(job.m_key);
		if(it != m_inFlight.end())
		{
			// Already queued: promote an urgent request to the front.
			if(job.m_pr == Priority::NOW)
			{
				it->second = true; // notify when it lands, even if running
				for(auto q = m_queue.begin(); q != m_queue.end(); ++q)
				{
					if(q->m_key == job.m_key)
					{
						Job j = std::move(*q);
						j.m_pr = Priority::NOW;
						m_queue.erase(q);
						m_queue.push_front(std::move(j));
						break;
					}
				}
			}
			return;
		}
		m_inFlight[job.m_key] = job.m_pr == Priority::NOW;
		// Newest urgent request first (the user moved on); prefetch in order at
		// the back.
		if(job.m_pr == Priority::NOW)
		{
			m_queue.push_front(std::move(job));
		}
		else
		{
			m_queue.push_back(std::move(job));
		}
	}
	m_cv.notify_one();
}

void ImageService::put(const std::string& key, CacheEntry entry)
{
	// m_mu held.
	if(auto it = m_cache.find(key); it != m_cache.end())
	{
		m_bytes -= it->second.m_bytes;
		m_lru.erase(it->second.m_lru);
		m_cache.erase(it);
	}
	m_lru.push_front(key);
	entry.m_lru = m_lru.begin();
	m_bytes += entry.m_bytes;
	m_cache.emplace(key, std::move(entry));
	while(m_bytes > m_budget && m_lru.size() > 1)
	{
		auto& victim = m_lru.back();
		auto it = m_cache.find(victim);
		m_bytes -= it->second.m_bytes;
		m_cache.erase(it);
		m_lru.pop_back();
	}
}

ImageInfoPtr ImageService::infoCached(const std::filesystem::path& p,
                                      const std::string& ident)
{
	const std::string key = "I|" + ident;
	{
		std::lock_guard lk(m_mu);
		if(auto v = get<ImageInfo>(key))
		{
			return v;
		}
	}
	auto info = std::make_shared<const ImageInfo>(probeImage(p));
	std::lock_guard lk(m_mu);
	put(key, CacheEntry{info, 4096, {}});
	return info;
}

LayerImagePtr ImageService::layerCached(const std::filesystem::path& p,
                                        const std::string& ident,
                                        const std::string& label,
                                        int reduce)
{
	const std::string key = layerKey(ident, label, reduce);
	{
		std::lock_guard lk(m_mu);
		if(auto v = get<LayerImage>(key))
		{
			return v;
		}
	}
	ImageInfoPtr info = infoCached(p, ident);
	int li = info->findLayer(label);
	auto img = loadLayer(*info, li < 0 ? 0 : li);
	if(reduce > 1)
	{
		img = downsample(img, reduce);
	}
	auto ptr = std::make_shared<const LayerImage>(std::move(img));
	std::lock_guard lk(m_mu);
	put(key, CacheEntry{ptr, ptr->bytes(), {}});
	return ptr;
}

void ImageService::run()
{
	for(;;)
	{
		Job job;
		{
			std::unique_lock lk(m_mu);
			m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
			if(m_stop)
			{
				return;
			}
			job = std::move(m_queue.front());
			m_queue.pop_front();
		}
		try
		{
			auto t0 = std::chrono::steady_clock::now();
			if(job.m_diffA)
			{
				// Full resolution both sides, so the pixels line up.
				auto a =
				    layerCached(*job.m_diffA, job.m_identA, *job.m_layer, 1);
				auto b = layerCached(job.m_path, job.m_identB, *job.m_layer, 1);
				auto img = diffLayers(*a, *b);
				if(job.m_reduce > 1)
				{
					img = downsample(img, job.m_reduce);
				}
				auto ptr = std::make_shared<const LayerImage>(std::move(img));
				std::lock_guard lk(m_mu);
				put(job.m_key, CacheEntry{ptr, ptr->bytes(), {}});
			}
			else if(job.m_layer)
			{
				layerCached(job.m_path, job.m_ident, *job.m_layer, job.m_reduce);
			}
			else
			{
				infoCached(job.m_path, job.m_ident);
			}
			spdlog::debug("loaded {} in {} ms",
			              job.m_key,
			              std::chrono::duration_cast<std::chrono::milliseconds>(
			                  std::chrono::steady_clock::now() - t0)
			                  .count());
		}
		catch(const std::exception& e)
		{
			spdlog::warn("load failed {}: {}", job.m_path.string(), e.what());
			std::lock_guard lk(m_mu);
			m_errors[job.m_ident] = e.what();
		}
		bool waited;
		{
			std::lock_guard lk(m_mu);
			auto it = m_inFlight.find(job.m_key);
			waited = it != m_inFlight.end() && it->second;
			m_inFlight.erase(job.m_key);
		}
		if(waited && m_notify)
		{
			m_notify();
		}
	}
}

} // namespace rv
