#include "pch.h"
#include "Cemu_UWP_HostMain.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

using namespace Cemu_UWP_Host;
using namespace concurrency;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace
{
std::string ToUtf8(Platform::String^ value)
{
	if (!value) return {};
	const auto length = WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, nullptr, 0, nullptr, nullptr);
	std::string result(length > 0 ? length : 0, '\0');
	if (length > 1) WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, &result[0], length, nullptr, nullptr);
	if (!result.empty()) result.pop_back();
	return result;
}

Platform::String^ FromUtf8(const char* value)
{
	if (!value || !*value) return "";
	const auto length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
	if (length <= 1) return "";
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value, -1, &result[0], length);
	result.pop_back();
	return ref new Platform::String(result.c_str(), static_cast<unsigned int>(result.size()));
}

struct BrokeredFile
{
	StorageFile^ file;
	uint64_t size{};
	// Title installs read each file sequentially.  A single prefetched block can
	// overlap the next Storage broker read with Cemu's local-file write.  The
	// generic launcher and graphic-pack importer intentionally keep their old
	// behavior.
	bool sequentialPrefetch{};
};

struct BrokeredStream
{
	IRandomAccessStream^ stream;
	DataReader^ reader;
	Windows::Foundation::IAsyncOperation<unsigned int>^ pendingRead;
	uint64_t pendingOffset{};
	uint64_t length{};
	bool sequentialPrefetch{};
};

struct CachedTitleEntry
{
	std::string relativePath;
	CemuEmbedBrokeredEntryType type{};
	uint64_t size{};
	StorageFile^ file{};
};

struct TitleInstallBroker
{
	std::vector<CachedTitleEntry> entries;
	std::unordered_map<std::wstring, StorageFolder^> destinationFolders;
	bool cacheReady{};
	bool fastCopyDisabled{};
	std::function<void(uint64_t, uint64_t, const std::string&)> progressCallback;
	std::chrono::steady_clock::time_point lastProgressUpdate{};
};

CemuEmbedResult EnumerateFolder(StorageFolder^ folder, const std::string& relativePath,
	CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData)
{
	constexpr unsigned int pageSize = 256;
	unsigned int startIndex = 0;
	for (;;)
	{
		const auto items = create_task(folder->GetItemsAsync(startIndex, pageSize)).get();
		for (const auto& item : items)
		{
			const auto name = ToUtf8(item->Name);
			const auto itemPath = relativePath.empty() ? name : relativePath + "/" + name;
			if (auto childFolder = dynamic_cast<StorageFolder^>(item))
			{
				if (entryCallback(entryCallbackUserData, itemPath.c_str(), CEMU_EMBED_BROKERED_DIRECTORY, 0, nullptr) != CEMU_EMBED_OK)
					return CEMU_EMBED_STORAGE_FAILED;
				const auto result = EnumerateFolder(childFolder, itemPath, entryCallback, entryCallbackUserData);
				if (result != CEMU_EMBED_OK) return result;
			}
			else if (auto file = dynamic_cast<StorageFile^>(item))
			{
				auto properties = create_task(file->GetBasicPropertiesAsync()).get();
				BrokeredFile brokeredFile{ file, properties->Size, false };
				const auto result = entryCallback(entryCallbackUserData, itemPath.c_str(), CEMU_EMBED_BROKERED_FILE, properties->Size, &brokeredFile);
				if (result != CEMU_EMBED_OK) return result;
			}
		}
		if (items->Size < pageSize) break;
		startIndex += items->Size;
	}
	return CEMU_EMBED_OK;
}

CemuEmbedResult CacheTitleFolder(StorageFolder^ folder, const std::string& relativePath,
	std::vector<CachedTitleEntry>& entries)
{
	constexpr unsigned int pageSize = 256;
	unsigned int startIndex = 0;
	for (;;)
	{
		const auto items = create_task(folder->GetItemsAsync(startIndex, pageSize)).get();
		std::vector<std::pair<std::string, StorageFile^>> files;
		files.reserve(items->Size);
		for (const auto& item : items)
		{
			const auto name = ToUtf8(item->Name);
			const auto itemPath = relativePath.empty() ? name : relativePath + "/" + name;
			if (auto childFolder = dynamic_cast<StorageFolder^>(item))
			{
				entries.push_back({ itemPath, CEMU_EMBED_BROKERED_DIRECTORY, 0, nullptr });
				const auto result = CacheTitleFolder(childFolder, itemPath, entries);
				if (result != CEMU_EMBED_OK) return result;
			}
			else if (auto file = dynamic_cast<StorageFile^>(item))
			{
				files.emplace_back(itemPath, file);
			}
		}

		// Storage metadata calls are brokered IPC operations. Issue them together
		// per page instead of waiting for every file serially. The resulting file
		// handles and sizes are retained for the DLL's second enumeration pass.
		if (!files.empty())
		{
			// Storage broker requests are IPC on Xbox. Large fan-out batches cause
			// throttling and make an install slower even though more operations are
			// nominally in flight. Eight requests keep the storage queue busy without
			// forcing dozens of simultaneous BasicProperties allocations.
			constexpr size_t metadataBatchSize = 8;
			for (size_t batchStart = 0; batchStart < files.size(); batchStart += metadataBatchSize)
			{
				const auto batchEnd = (std::min)(batchStart + metadataBatchSize, files.size());
				std::vector<task<Windows::Storage::FileProperties::BasicProperties^>> tasks;
				tasks.reserve(batchEnd - batchStart);
				for (size_t index = batchStart; index < batchEnd; ++index)
					tasks.emplace_back(create_task(files[index].second->GetBasicPropertiesAsync()));
				const auto properties = when_all(tasks.begin(), tasks.end()).get();
				for (size_t index = batchStart; index < batchEnd; ++index)
					entries.push_back({ files[index].first, CEMU_EMBED_BROKERED_FILE,
						properties[index - batchStart]->Size, files[index].second });
			}
		}

		if (items->Size < pageSize) break;
		startIndex += items->Size;
	}
	return CEMU_EMBED_OK;
}

CemuEmbedResult CEMU_EMBED_CALL EnumerateCachedTitleFolder(void* userData, void* folderHandle,
	CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData)
{
	if (!userData || !folderHandle || !entryCallback)
		return CEMU_EMBED_INVALID_ARGUMENT;
	try
	{
		auto& broker = *static_cast<TitleInstallBroker*>(userData);
		if (!broker.cacheReady)
		{
			broker.entries.clear();
			const auto result = CacheTitleFolder(reinterpret_cast<StorageFolder^>(folderHandle), {}, broker.entries);
			if (result != CEMU_EMBED_OK) return result;
			broker.cacheReady = true;
		}
		for (const auto& entry : broker.entries)
		{
			if (entry.type == CEMU_EMBED_BROKERED_DIRECTORY)
			{
				if (entryCallback(entryCallbackUserData, entry.relativePath.c_str(), entry.type,
					0, nullptr) != CEMU_EMBED_OK)
					return CEMU_EMBED_STORAGE_FAILED;
				continue;
			}
			BrokeredFile brokeredFile{ entry.file, entry.size, true };
			if (entryCallback(entryCallbackUserData, entry.relativePath.c_str(), entry.type,
				entry.size, &brokeredFile) != CEMU_EMBED_OK)
				return CEMU_EMBED_STORAGE_FAILED;
		}
		return CEMU_EMBED_OK;
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

void CEMU_EMBED_CALL CachedTitleProgress(void* userData, uint64_t bytesCopied,
	uint64_t totalBytes, const char* relativePath)
{
	auto broker = static_cast<TitleInstallBroker*>(userData);
	if (!broker || !broker->progressCallback)
		return;

	// Progress is emitted once per copied block by the DLL. Posting every one
	// to the XAML dispatcher can enqueue thousands of UI tasks for a large game
	// and competes with the Storage broker on Xbox. Always deliver completion,
	// otherwise cap the title-install UI to four updates per second.
	const auto now = std::chrono::steady_clock::now();
	if (bytesCopied != totalBytes && broker->lastProgressUpdate.time_since_epoch().count() &&
		now - broker->lastProgressUpdate < std::chrono::milliseconds(250))
		return;
	broker->lastProgressUpdate = now;
	broker->progressCallback(bytesCopied, totalBytes,
		relativePath ? relativePath : "");
}

CemuEmbedResult CEMU_EMBED_CALL CopyCachedTitleFile(void* userData, void* fileHandle,
	const char* destinationPathUtf8)
{
	if (!userData || !fileHandle || !destinationPathUtf8)
		return CEMU_EMBED_INVALID_ARGUMENT;
	auto& broker = *static_cast<TitleInstallBroker*>(userData);
	if (broker.fastCopyDisabled)
		return CEMU_EMBED_INVALID_STATE;

	try
	{
		auto brokeredFile = static_cast<BrokeredFile*>(fileHandle);
		if (!brokeredFile->file)
			return CEMU_EMBED_INVALID_ARGUMENT;
		const auto destinationPath = FromUtf8(destinationPathUtf8);
		std::wstring fullPath(destinationPath->Data(), destinationPath->Length());
		std::replace(fullPath.begin(), fullPath.end(), L'/', L'\\');
		const auto separator = fullPath.find_last_of(L'\\');
		if (separator == std::wstring::npos || separator + 1 >= fullPath.size())
			return CEMU_EMBED_INVALID_ARGUMENT;
		const auto folderPath = fullPath.substr(0, separator);
		const auto fileName = fullPath.substr(separator + 1);

		StorageFolder^ destinationFolder = nullptr;
		const auto cached = broker.destinationFolders.find(folderPath);
		if (cached != broker.destinationFolders.end())
			destinationFolder = cached->second;
		else
		{
			destinationFolder = create_task(StorageFolder::GetFolderFromPathAsync(
				ref new Platform::String(folderPath.c_str()))).get();
			broker.destinationFolders.emplace(folderPath, destinationFolder);
		}
		if (!destinationFolder)
			return CEMU_EMBED_STORAGE_FAILED;

		create_task(brokeredFile->file->CopyAsync(destinationFolder,
			ref new Platform::String(fileName.c_str()),
			NameCollisionOption::ReplaceExisting)).get();
		return CEMU_EMBED_OK;
	}
	catch (...)
	{
		// Some providers do not implement broker-to-app CopyAsync. Disable the
		// fast path for the rest of this install and let Cemu use chunked reads.
		broker.fastCopyDisabled = true;
		broker.destinationFolders.clear();
		return CEMU_EMBED_STORAGE_FAILED;
	}
}
}

Cemu_UWP_HostMain::Cemu_UWP_HostMain(HWND window, const CemuEmbedD3D11Surface& surface, int width, int height, double dpiScale)
	: m_window(window), m_d3d11Surface(surface), m_width(width), m_height(height), m_dpiScale(dpiScale) {}

Cemu_UWP_HostMain::~Cemu_UWP_HostMain() { Stop(); }

bool Cemu_UWP_HostMain::Start()
{
	if (m_started) return true;
	const auto installed = Windows::ApplicationModel::Package::Current->InstalledLocation->Path;
	const auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
	const auto cache = Windows::Storage::ApplicationData::Current->LocalCacheFolder->Path;
	const auto executable = ToUtf8(installed);
	const auto userData = ToUtf8(local);
	const auto cachePath = ToUtf8(cache);
	CemuEmbedConfig config{ sizeof(config), CEMU_EMBED_ABI_VERSION, executable.c_str(), userData.c_str(), userData.c_str(), cachePath.c_str(), executable.c_str() };
	CemuEmbedCallbacks callbacks{ sizeof(callbacks), this, Log, Error, StateChanged };
	if (CemuEmbed_Create(&config, &callbacks, &m_instance) != CEMU_EMBED_OK)
		return false;
	// The Xbox host has no wxWidgets USB manager, so expose Cemu's native
	// Dimensions Toy Pad for the entire embedded session.
	if (CemuEmbed_EnableDimensionsToypad(m_instance, 1) != CEMU_EMBED_OK)
	{
		CemuEmbed_Destroy(m_instance);
		m_instance = nullptr;
		return false;
	}
	CemuEmbedSurface surface{ sizeof(surface), m_window, &m_d3d11Surface, m_width, m_height, m_dpiScale };
	if (CemuEmbed_SetSurface(m_instance, &surface) != CEMU_EMBED_OK || CemuEmbed_InitializeAsync(m_instance) != CEMU_EMBED_OK)
	{
		CemuEmbed_Destroy(m_instance);
		m_instance = nullptr;
		return false;
	}
	m_started = true;
	return true;
}

bool Cemu_UWP_HostMain::LaunchGame(StorageFolder^ gameFolder)
{
	if (!m_instance || !gameFolder) return false;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, this,
		EnumerateBrokeredFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, BrokeredProgress, nullptr
	};
	return CemuEmbed_LaunchGameFromBrokeredFolder(m_instance, reinterpret_cast<void*>(gameFolder), &storage) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::LaunchGameFile(StorageFile^ gameFile)
{
	if (!m_instance || !gameFile || !gameFile->Path || gameFile->Path->IsEmpty())
		return false;
	return LaunchGamePath(ToUtf8(gameFile->Path));
}

bool Cemu_UWP_HostMain::LaunchGamePath(const std::string& gamePath)
{
	return m_instance && !gamePath.empty() &&
		CemuEmbed_LaunchGame(m_instance, gamePath.c_str()) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::InstallTitle(StorageFolder^ titleFolder,
	CemuEmbedInstallType expectedType, uint64_t* installedBaseTitleId)
{
	if (!m_instance || !titleFolder) return false;
	TitleInstallBroker broker;
	broker.progressCallback = m_progressCallback;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, &broker,
		EnumerateCachedTitleFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, CachedTitleProgress, CopyCachedTitleFile
	};
	return CemuEmbed_InstallTitleFromBrokeredFolder(m_instance,
		reinterpret_cast<void*>(titleFolder), &storage, expectedType,
		installedBaseTitleId) == CEMU_EMBED_OK;
}

std::vector<InstalledTitle> Cemu_UWP_HostMain::GetInstalledTitles()
{
	std::vector<InstalledTitle> titles;
	if (m_instance)
		CemuEmbed_EnumerateInstalledTitles(m_instance, InstalledTitleFound, &titles);
	return titles;
}

bool Cemu_UWP_HostMain::GetActiveAccount(ActiveAccount& account)
{
	if (!m_instance) return false;
	CemuEmbedActiveAccount embedded{};
	embedded.struct_size = sizeof(embedded);
	embedded.abi_version = CEMU_EMBED_ACCOUNT_VERSION;
	if (CemuEmbed_GetActiveAccount(m_instance, &embedded) != CEMU_EMBED_OK)
		return false;
	account.persistentId = embedded.persistent_id;
	account.onlineEnabled = embedded.online_enabled != 0;
	account.miiName = embedded.mii_name_utf8;
	account.accountId = embedded.account_id_utf8;
	return true;
}

bool Cemu_UWP_HostMain::LaunchInstalledTitle(uint64_t baseTitleId)
{
	return m_instance &&
		CemuEmbed_LaunchInstalledTitle(m_instance, baseTitleId) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::InstallGraphicPacks(StorageFolder^ graphicPacksFolder,
	uint32_t* importedPackCount)
{
	if (!m_instance || !graphicPacksFolder) return false;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, this,
		EnumerateBrokeredFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, BrokeredProgress, nullptr
	};
	return CemuEmbed_InstallGraphicPacksFromBrokeredFolder(m_instance,
		reinterpret_cast<void*>(graphicPacksFolder), &storage,
		importedPackCount) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::SetGraphicPacksEnabledForTitle(uint64_t baseTitleId,
	bool enabled, uint32_t* affectedPackCount)
{
	return m_instance && CemuEmbed_SetGraphicPacksEnabledForTitle(m_instance,
		baseTitleId, enabled ? 1 : 0, affectedPackCount) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::ApplySafeGraphicPackPolicyForTitle(uint64_t baseTitleId,
	uint32_t* affectedPackCount)
{
	return m_instance &&
		CemuEmbed_ApplySafeGraphicPackPolicyForTitle(m_instance, baseTitleId,
			affectedPackCount) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::EnsureDefaultGamepadProfile()
{
	if (!m_instance) return false;
	int32_t ready{};
	return CemuEmbed_EnsureDefaultGamepadProfile(m_instance, &ready) ==
		CEMU_EMBED_OK && ready != 0;
}

bool Cemu_UWP_HostMain::SetGamepadState(const CemuEmbedGamepadState& state)
{
	return m_instance && CemuEmbed_SetHostGamepadState(m_instance, &state) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::SetVirtualMouse(int x, int y, bool leftDown, bool enabled)
{
	return m_instance &&
		CemuEmbed_SetVirtualMouse(m_instance, x, y,
			leftDown ? 1 : 0, enabled ? 1 : 0) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::SetPerformanceMetrics(bool enabled)
{
	return m_instance &&
		CemuEmbed_SetPerformanceMetrics(m_instance, enabled ? 1 : 0) == CEMU_EMBED_OK;
}

std::vector<DimensionsFigure> Cemu_UWP_HostMain::GetDimensionsFigures()
{
	std::vector<DimensionsFigure> figures;
	if (m_instance)
		CemuEmbed_EnumerateDimensionsFigures(m_instance, DimensionsFigureFound, &figures);
	return figures;
}

bool Cemu_UWP_HostMain::PlaceDimensionsFigure(uint32_t figureId, uint8_t slot)
{
	return m_instance &&
		CemuEmbed_PlaceDimensionsFigure(m_instance, figureId, slot) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::RemoveDimensionsFigure(uint8_t slot)
{
	return m_instance &&
		CemuEmbed_RemoveDimensionsFigure(m_instance, slot) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::MoveDimensionsFigure(uint8_t sourceSlot, uint8_t destinationSlot)
{
	return m_instance && CemuEmbed_MoveDimensionsFigure(
		m_instance, sourceSlot, destinationSlot) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::ImportKeys(const std::vector<uint8_t>& data,
	uint32_t* validKeyCount)
{
	if (!m_instance || data.empty())
		return false;
	return CemuEmbed_ImportKeys(m_instance, data.data(),
		static_cast<uint32_t>(data.size()), validKeyCount) == CEMU_EMBED_OK;
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::InstalledTitleFound(
	void* userData, const CemuEmbedInstalledTitle* title)
{
	if (!userData || !title ||
		title->struct_size < sizeof(CemuEmbedInstalledTitle) ||
		title->abi_version != CEMU_EMBED_LIBRARY_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	auto& titles = *static_cast<std::vector<InstalledTitle>*>(userData);
	titles.push_back({
		title->title_id,
		title->base_version,
		title->effective_version,
		title->update_version,
		title->dlc_version,
		title->dlc_count,
		title->region,
		title->compatible_graphic_pack_count,
		title->enabled_graphic_pack_count,
		title->name_utf8 ? title->name_utf8 : "",
		title->region_utf8 ? title->region_utf8 : ""
	});
	return CEMU_EMBED_OK;
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::DimensionsFigureFound(
	void* userData, const CemuEmbedDimensionsFigure* figure)
{
	if (!userData || !figure ||
		figure->struct_size < sizeof(CemuEmbedDimensionsFigure) ||
		figure->abi_version != CEMU_EMBED_DIMENSIONS_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	auto& figures = *static_cast<std::vector<DimensionsFigure>*>(userData);
	figures.push_back({
		figure->id,
		figure->type == CEMU_EMBED_DIMENSIONS_VEHICLE_OR_GADGET,
		figure->name_utf8 ? figure->name_utf8 : ""
	});
	return CEMU_EMBED_OK;
}

void Cemu_UWP_HostMain::SetDiagnosticCallback(std::function<void(const std::string&)> callback)
{
	m_diagnosticCallback = std::move(callback);
}

void Cemu_UWP_HostMain::SetStateCallback(std::function<void(CemuEmbedState)> callback)
{
	m_stateCallback = std::move(callback);
}

void Cemu_UWP_HostMain::SetProgressCallback(std::function<void(uint64_t, uint64_t, const std::string&)> callback)
{
	m_progressCallback = std::move(callback);
}

bool Cemu_UWP_HostMain::IsReady() const
{
	return m_ready.load(std::memory_order_acquire);
}

void Cemu_UWP_HostMain::ResizeSurface(int width, int height, double dpiScale)
{
	m_width = (std::max)(width, 1);
	m_height = (std::max)(height, 1);
	m_dpiScale = dpiScale > 0.0 ? dpiScale : 1.0;
	if (!m_instance) return;
	CemuEmbedSurface surface{ sizeof(surface), m_window, &m_d3d11Surface, m_width, m_height, m_dpiScale };
	CemuEmbed_SetSurface(m_instance, &surface);
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::EnumerateBrokeredFolder(void*, void* folderHandle,
	CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData)
{
	try
	{
		return EnumerateFolder(reinterpret_cast<StorageFolder^>(folderHandle), {}, entryCallback, entryCallbackUserData);
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::OpenBrokeredFile(void*, void* fileHandle, void** streamHandle)
{
	if (!fileHandle || !streamHandle) return CEMU_EMBED_INVALID_ARGUMENT;
	try
	{
		auto brokeredFile = static_cast<BrokeredFile*>(fileHandle);
		auto file = brokeredFile->file;
		auto stream = create_task(file->OpenAsync(FileAccessMode::Read)).get();
		auto reader = ref new DataReader(stream);

		// The Xbox storage broker has higher per-request latency than desktop UWP.
		// Title installation is sequential, so it can keep one next request in
		// flight while Cemu writes the current block. Graphic-pack imports retain
		// their original synchronous callback behavior.
		constexpr uint64_t kPrefetchMinimumSize = 4ull * 1024 * 1024;
		const bool useSequentialPrefetch =
			brokeredFile->sequentialPrefetch && brokeredFile->size >= kPrefetchMinimumSize;
		reader->InputStreamOptions = useSequentialPrefetch
			? InputStreamOptions::None
			: InputStreamOptions::Partial;
		*streamHandle = new BrokeredStream{
			stream,
			reader,
			nullptr,
			0,
			brokeredFile->size,
			useSequentialPrefetch
		};
		return CEMU_EMBED_OK;
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::ReadBrokeredStream(void*, void* streamHandle, uint64_t offset,
	uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead)
{
	if (!streamHandle || !buffer || !bytesRead) return CEMU_EMBED_INVALID_ARGUMENT;
	try
	{
		auto brokeredStream = static_cast<BrokeredStream*>(streamHandle);
		auto stream = brokeredStream->stream;
		auto reader = brokeredStream->reader;

		// A non-sequential request cannot consume a read-ahead block. Complete and
		// drain it first so the shared DataReader is returned to a consistent state.
		if (brokeredStream->pendingRead && brokeredStream->pendingOffset != offset)
		{
			const auto discardedCount = create_task(brokeredStream->pendingRead).get();
			brokeredStream->pendingRead = nullptr;
			if (discardedCount)
			{
				std::vector<unsigned char> discarded(discardedCount);
				reader->ReadBytes(Platform::ArrayReference<unsigned char>(discarded.data(), discardedCount));
			}
		}

		if (!brokeredStream->pendingRead)
		{
			if (stream->Position != offset) stream->Seek(offset);
			brokeredStream->pendingRead = reader->LoadAsync(bufferSize);
			brokeredStream->pendingOffset = offset;
		}

		const auto count = create_task(brokeredStream->pendingRead).get();
		brokeredStream->pendingRead = nullptr;
		if (count)
			reader->ReadBytes(Platform::ArrayReference<unsigned char>(buffer, count));
		*bytesRead = count;

		// Bound the pipeline to a single Cemu staging block. Cemu's installation
		// path uses 2 MiB blocks, which is large enough to amortize Xbox broker
		// latency without the memory pressure caused by deeper queues.
		const uint64_t nextOffset = offset + count;
		if (brokeredStream->sequentialPrefetch && count && nextOffset < brokeredStream->length)
		{
			const auto remaining = brokeredStream->length - nextOffset;
			const auto nextCount = static_cast<uint32_t>(std::min<uint64_t>(bufferSize, remaining));
			if (nextCount)
			{
				if (stream->Position != nextOffset) stream->Seek(nextOffset);
				brokeredStream->pendingRead = reader->LoadAsync(nextCount);
				brokeredStream->pendingOffset = nextOffset;
			}
		}
		return CEMU_EMBED_OK;
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

void __cdecl Cemu_UWP_HostMain::CloseBrokeredStream(void*, void* streamHandle)
{
	auto brokeredStream = static_cast<BrokeredStream*>(streamHandle);
	if (brokeredStream && brokeredStream->pendingRead)
		brokeredStream->pendingRead->Cancel();
	delete brokeredStream;
}

void __cdecl Cemu_UWP_HostMain::BrokeredProgress(void* userData, uint64_t bytesCopied,
	uint64_t totalBytes, const char* relativePath)
{
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (self && self->m_progressCallback)
		self->m_progressCallback(bytesCopied, totalBytes, relativePath ? relativePath : "");
}

void Cemu_UWP_HostMain::Pump() { if (m_instance) CemuEmbed_Pump(m_instance); }

void Cemu_UWP_HostMain::Stop()
{
	if (!m_instance) return;
	CemuEmbed_Destroy(m_instance);
	m_instance = nullptr;
	m_started = false;
}

void __cdecl Cemu_UWP_HostMain::Log(void*, const char* category, const char* message)
{
	std::string line = "[Cemu";
	if (category && *category)
	{
		line += "/";
		line += category;
	}
	line += "] ";
	if (message) line += message;
	line += "\r\n";
	OutputDebugStringA(line.c_str());
}

void __cdecl Cemu_UWP_HostMain::Error(void* userData, CemuEmbedResult result, const char* message)
{
	std::string line = "[Cemu error ";
	line += std::to_string(static_cast<int>(result));
	line += "] ";
	if (message) line += message;
	line += "\r\n";
	OutputDebugStringA(line.c_str());
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (!self) return;
	if (self->m_diagnosticCallback) self->m_diagnosticCallback(line);
	if (result == CEMU_EMBED_INITIALIZATION_FAILED)
	{
		self->m_ready.store(false, std::memory_order_release);
		if (self->m_stateCallback) self->m_stateCallback(CEMU_EMBED_STATE_FAILED);
	}
}
void __cdecl Cemu_UWP_HostMain::StateChanged(void* userData, CemuEmbedState state)
{
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (!self) return;
	self->m_ready.store(state == CEMU_EMBED_STATE_READY, std::memory_order_release);
	if (self->m_stateCallback) self->m_stateCallback(state);
}
