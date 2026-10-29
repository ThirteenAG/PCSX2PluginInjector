// Included after the injector lifecycle helpers; all operations run on the CPU thread.
namespace
{
    std::vector<LoadedGuestModule> s_pendingGuestState;
    uint32_t s_pendingStateDisc = 0, s_pendingStateElf = 0;
    bool s_pendingInstalledSetDiffers = false;

    std::filesystem::path GuestPluginsDirectory()
    {
        return std::filesystem::path(GetThisModulePath<std::wstring>()).parent_path() / L"PLUGINS";
    }
    std::string GuestStateName(const std::filesystem::path& path)
    {
        const auto name = path.lexically_relative(GuestPluginsDirectory()).generic_u8string();
        return std::string(name.begin(), name.end());
    }
    void PrepareGuestPluginState(std::span<const uint8_t> bytes)
    {
        s_pendingGuestState.clear();
        s_pendingInstalledSetDiffers = false;
        const auto manifest = GuestPluginState::Decode(bytes);
        std::vector<LoadedGuestModule> pending;
        const auto directory = GuestPluginsDirectory();
        for (const auto& record : manifest.modules)
        {
            const auto path = directory / std::filesystem::path(std::u8string(record.name.begin(), record.name.end()));
            // Version 1 predates embedded ELFs; an exact original is needed once to upgrade it.
            auto file = record.file;
            if (manifest.version == 1)
            {
                file = LoadFileToBuffer(path);
                if (GuestPluginState::Hash(file) != record.hash)
                    throw std::runtime_error("Old plugin state needs its original ELF to upgrade: " + record.name);
            }
            GuestModule::Image image;
            std::string error;
            const bool legacy = GuestModule::IsLegacy(file);
            if (!(legacy ? GuestModule::LoadLegacy(file, image, error) :
                GuestModule::Load(file, record.base, GuestModule::ArenaEnd, image, error)) ||
                image.info.Base != record.base || image.info.Size != record.size || image.info.EntryPoint != record.entry)
                throw std::runtime_error("Invalid saved plugin layout: " + record.name);
            pending.push_back({path, std::move(image), record.hash, std::move(file)});
        }
        // Disk contents affect only the informational note, never the restored registrations.
        bool differs = false;
        try
        {
            std::vector<std::string> installed;
            if (std::filesystem::exists(directory))
                for (const auto& item : std::filesystem::recursive_directory_iterator(directory))
                {
                    if (!item.is_regular_file() || !iequals(item.path().extension().wstring(), L".elf") ||
                        iequals(item.path().filename().wstring(), L"PCSX2PluginInvoker.elf")) continue;
                    const auto name = GuestStateName(item.path());
                    const auto saved = std::find_if(manifest.modules.begin(), manifest.modules.end(),
                        [&](const auto& record) { return record.name == name; });
                    const auto file = LoadFileToBuffer(item.path());
                    if (saved != manifest.modules.end())
                    {
                        installed.push_back(name);
                        differs |= GuestPluginState::Hash(file) != saved->hash;
                        continue;
                    }
                    GuestModule::Image probe;
                    std::string error;
                    if (!(GuestModule::IsLegacy(file) ? GuestModule::LoadLegacy(file, probe, error) :
                        GuestModule::Load(file, GuestModule::ArenaBegin, GuestModule::ArenaEnd, probe, error))) continue;
                    const auto& info = probe.info;
                    if (probe.matchesCRC(info.CompatibleCRCListAddr, info.CompatibleCRCListSize, manifest.disc) &&
                        (!info.CompatibleElfCRCListAddr || probe.matchesCRC(info.CompatibleElfCRCListAddr, info.CompatibleElfCRCListSize, manifest.elf)))
                        differs = true;
                }
            differs |= installed.size() != manifest.modules.size();
        }
        catch (const std::exception&)
        {
            differs = true; // Unreadable installed files cannot prevent a self-contained restore.
        }
        s_pendingStateDisc = manifest.disc; s_pendingStateElf = manifest.elf;
        s_pendingInstalledSetDiffers = differs;
        s_pendingGuestState = std::move(pending);
    }
    void RestoreGuestPluginState(const uintptr_t* context)
    {
        bool unthrottle = false;
        for (const auto& module : s_pendingGuestState)
        {
            const auto& image = module.image;
            const auto& mod = image.info;
            std::error_code settingsError;
            const auto settingsPath = GuestSettingsPath(module.path, image);
            const bool haveSettings = std::filesystem::is_regular_file(settingsPath, settingsError);
            if (mod.PCSX2DataAddr)
            {
                auto [desktopWidth, desktopHeight] = GetDesktopRes();
                const uint32_t values[] = {static_cast<uint32_t>(desktopWidth), static_cast<uint32_t>(desktopHeight),
                    static_cast<uint32_t>(context[3]), static_cast<uint32_t>(context[4]),
                    static_cast<uint32_t>(context[5]), static_cast<uint32_t>(context[6]), s_hostSupportsGuestRenderPhase};
                WriteMemoryRaw(mod.PCSX2DataAddr, values, std::min<uint32_t>(mod.PCSX2DataSize, sizeof(values)));
            }
            if (mod.PluginDataAddr && haveSettings)
            {
                auto settings = LoadFileToBuffer(settingsPath);
                settings.resize(std::min<size_t>(settings.size(), mod.PluginDataSize - 4));
                MemoryFill(mod.PluginDataAddr, 0, mod.PluginDataSize);
                WriteMemory32(mod.PluginDataAddr, static_cast<uint32_t>(settings.size()));
                WriteMemoryRaw(mod.PluginDataAddr + 4, settings.data(), static_cast<uint32_t>(settings.size()));
            }
            auto writeSymbol = [&](const char* name, uint32_t value) {
                const auto* symbol = image.find(name);
                if (symbol && symbol->size == 4 && !(symbol->address & 3) && image.contains(symbol->address, 4, 1))
                    WriteMemory32(symbol->address, value);
            };
            writeSymbol("PCSX2FSettingsVersion", s_hostSupportsSettings ? (UsesCfg(image) ? 2u : 1u) : 0u);
            writeSymbol("PCSX2FSettingsPeerVersion", s_hostSupportsSettings && UsesCfg(image) ? 1u : 0u);
            if (const auto* symbol = image.find("PCSX2FStateLoadGeneration"); symbol && symbol->size == 4 &&
                !(symbol->address & 3) && image.contains(symbol->address, 4, 1))
            {
                uint32_t generation;
                std::memcpy(&generation, reinterpret_cast<const void*>(gEEMainMemoryStart + symbol->address), 4);
                if (haveSettings) WriteMemory32(symbol->address, generation + 1);
            }
            else
                spd::log()->warn("{} has no post-state settings refresh export", module.path.filename().string());
            {
                std::lock_guard lock(s_inputMutex);
                for (const auto& [type, address, size] : {
                    std::tuple{PtrType::KeyboardData, mod.KeyboardStateAddr, mod.KeyboardStateSize},
                    std::tuple{PtrType::MouseData, mod.MouseStateAddr, mod.MouseStateSize},
                    std::tuple{PtrType::CheatStringData, mod.CheatStringAddr, mod.CheatStringSize}})
                {
                    if (!address) continue;
                    MemoryFill(address, 0, size);
                    InputData.emplace_back(type, address, gEEMainMemoryStart + address, size);
                }
            }
            if (mod.OSDTextAddr)
            {
                std::lock_guard lock(s_osdMutex);
                for (uint32_t offset = 0; offset < mod.OSDTextSize; offset += OSDStringSize)
                    GetOSDVector().emplace_back(reinterpret_cast<const char*>(gEEMainMemoryStart + mod.OSDTextAddr + offset), OSDStringSize);
            }
            if (mod.FrameLimitUnthrottleAddr && !unthrottle)
            {
                auto future = exitSignal.get_future();
                s_unthrottleThread = std::thread(&UnthrottleWatcher, std::move(future), reinterpret_cast<uint8_t*>(gEEMainMemoryStart + mod.FrameLimitUnthrottleAddr));
                unthrottle = true;
            }
        }
        {
            std::lock_guard lock(s_guestModuleMutex);
            s_loadedGuestModules = std::move(s_pendingGuestState);
            s_stateDisc = s_pendingStateDisc; s_stateElf = s_pendingStateElf;
        }
        if (!s_loadedGuestModules.empty())
        {
            StartWindowWatcher();
            if (HasInputPlugins()) EnsureInput();
            auto rain = GetModuleHandleW(L"PCSX2F.XboxRainDroplets64.asi");
            if (!rain)
            {
                rain = LoadLibraryW(L"PCSX2F.XboxRainDroplets64.asi");
                if (rain)
                    if (const auto initialize = reinterpret_cast<void (*)()>(GetProcAddress(rain, "InitializeASI"))) initialize();
            }
            if (rain) RegisterGuestRenderPhasePlugin(rain);
        }
    }
}

// 0: snapshot, 1: validate/stage, 2: suspend old consumers, 3: restore staged consumers.
// 4: discard staged state, 5: validate startup queue. Negative returns carry a bounded diagnostic.
CEXP int32_t GuestPluginStateTransfer(uint32_t operation, void* buffer, uint32_t size, char* error, uint32_t errorSize)
{
    try
    {
        switch (operation)
        {
        case 0:
        {
            GuestPluginState::Manifest manifest;
            manifest.disc = s_stateDisc; manifest.elf = s_stateElf;
            std::lock_guard lock(s_guestModuleMutex);
            for (const auto& module : s_loadedGuestModules)
                manifest.modules.push_back({GuestStateName(module.path), module.hash, module.image.info.Base, module.image.info.Size, module.image.info.EntryPoint, module.file});
            if (manifest.modules.empty()) throw std::runtime_error("No guest plugin identities are registered; cannot save active modules.");
            const auto bytes = GuestPluginState::Encode(manifest);
            if (bytes.size() > size) throw std::runtime_error("Plugin manifest buffer too small");
            std::memcpy(buffer, bytes.data(), bytes.size());
            return static_cast<int32_t>(bytes.size());
        }
        case 1: PrepareGuestPluginState({static_cast<const uint8_t*>(buffer), size}); return 0;
        case 2: ExitSignal(); ResetInput(); return 0;
        case 3:
        {
            // The host passes EE memory and its window-handle storage after RAM is restored.
            if (size != sizeof(uintptr_t) * 7) throw std::runtime_error("Invalid state restore context");
            const auto* context = static_cast<const uintptr_t*>(buffer);
            gEEMainMemoryStart = context[0]; gEEMainMemorySize = context[1];
            gWindowHandle = reinterpret_cast<const void*>(context[2]);
            RestoreGuestPluginState(context);
            return std::exchange(s_pendingInstalledSetDiffers, false) ? 1 : 0;
        }
        case 4: s_pendingGuestState.clear(); s_pendingInstalledSetDiffers = false; return 0;
        case 5:
        {
            if (!buffer || size % 16) throw std::runtime_error("Invalid guest startup queue");
            const auto* queue = static_cast<const uint32_t*>(buffer);
            uint32_t index = 0;
            std::vector<std::pair<uint32_t, uint32_t>> legacyBlocks;
            for (const auto& module : s_pendingGuestState)
            {
                const auto& image = module.image;
                const uint32_t entries[] = {image.legacyInit, image.info.EntryPoint};
                uint32_t legacyStack = 0, legacyContext = 0;
                for (const uint32_t entry : entries)
                {
                    if (!entry) continue;
                    if ((index + 1) * 16 > size || queue[index * 4] != entry || queue[index * 4 + 2])
                        throw std::runtime_error("Guest startup queue differs for " + module.path.filename().string());
                    const uint32_t stack = queue[index * 4 + 1], context = queue[index * 4 + 3];
                    if (!image.legacy)
                    {
                        if (stack != image.stackTop || context != image.context)
                            throw std::runtime_error("Guest startup context differs for " + module.path.filename().string());
                    }
                    else
                    {
                        if (uint64_t(context) + 32 + 64 * 1024 != stack || context < GuestModule::ArenaBegin || stack > GuestModule::ArenaEnd ||
                            (legacyStack && (legacyStack != stack || legacyContext != context)))
                            throw std::runtime_error("Invalid legacy plugin startup block");
                        for (const auto& other : s_pendingGuestState)
                            if (context < uint64_t(other.image.info.Base) + other.image.info.Size && other.image.info.Base < stack)
                                throw std::runtime_error("Legacy plugin startup block overlaps module code");
                        if (!legacyStack)
                        {
                            for (const auto& block : legacyBlocks)
                                if (context < block.second && block.first < stack) throw std::runtime_error("Overlapping legacy startup blocks");
                            legacyBlocks.emplace_back(context, stack);
                        }
                        legacyStack = stack; legacyContext = context;
                    }
                    ++index;
                }
            }
            if (index * 16 != size) throw std::runtime_error("Unexpected guest startup queue entries");
            return 0;
        }
        default: throw std::runtime_error("Unknown guest state operation");
        }
    }
    catch (const std::exception& exception)
    {
        if (error && errorSize) snprintf(error, errorSize, "%s", exception.what());
        return -1;
    }
}
