// v1 and v2 of {!BIND} are intentionally kept in one-file to keep things minimal.
//
// Expect the upcoming version to do some *limited* refactoring into sensible objects.

#include <SkyrimScripting/Plugin.h>
#include <json/json.h>

#include <Champollion/Pex/FileReader.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace SkyrimScripting::Bind {

    class GameStartedEvent : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent> {
    public:
        std::function<void()> callback;
        RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent*, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>* source) override {
            callback();
            source->RemoveEventSink(this);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    bool ProcessingDocStrings;
    std::vector<std::string> BindingLinesFromComments;
    Json::Value DocstringJsonRoot;
    std::filesystem::path DocstringJsonFilePath;
    RE::TESForm* DefaultBaseFormForCreatingObjects;
    RE::TESObjectREFR* LocationForPlacingObjects;
    std::string FilePath;
    unsigned int LineNumber;
    std::string ScriptName;
    RE::BSScript::IVirtualMachine* vm;
    GameStartedEvent GameStartedEventListener;
    constexpr auto BIND_COMMENT_PREFIX = "!BIND";
    constexpr auto DEFAULT_JSON = R"({"mtimes":{},"scripts":{}})";
    constexpr auto JSON_FILE_PATH = "Data\\SkyrimScripting\\Bind\\DocStrings.json";
    constexpr auto BINDING_FILES_FOLDER_ROOT = "Data\\Scripts\\Bindings";

    void LowerCase(std::string& text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    inline void ltrim(std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    }
    inline void rtrim(std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    }
    inline void trim(std::string& s) {
        rtrim(s);
        ltrim(s);
    }

    RE::TESForm* LookupFormID(RE::FormID formID) {
        auto* form = RE::TESForm::LookupByID(formID);
        if (form)
            return form;
        else
            logger::info("BIND ERROR [{}:{}] ({}) Form ID '{:x}' does not exist", FilePath, LineNumber, ScriptName, formID);
        return {};
    }
    RE::TESForm* LookupEditorID(const std::string& editorID) {
        auto* form = RE::TESForm::LookupByEditorID(editorID);
        if (form)
            return form;
        else
            logger::info("BIND ERROR [{}:{}] ({}) Form Editor ID '{}' does not exist (Are you using po3 Tweaks?)", FilePath, LineNumber, ScriptName, editorID);
        return {};
    }

    void Bind_Form(RE::TESForm* form) {
        auto handle = vm->GetObjectHandlePolicy()->GetHandleForObject(form->GetFormType(), form);
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        vm->CreateObject(ScriptName, object);
        vm->GetObjectBindPolicy()->BindObject(object, handle);
        logger::info("Bound form {:x} to {}", form->GetFormID(), ScriptName);
    }

    void Bind_GeneratedObject(RE::TESForm* baseForm = nullptr) {
        if (!baseForm) baseForm = DefaultBaseFormForCreatingObjects;  // By default, simply puts a fork next to the WEMerchantChest in the WEMerchantChests cell. Forking awesome.
        auto niPointer = LocationForPlacingObjects->PlaceObjectAtMe(skyrim_cast<RE::TESBoundObject*, RE::TESForm>(baseForm), false);
        if (niPointer)
            Bind_Form(niPointer.get());
        else
            logger::info("BIND ERROR [{}:{}] ({}) Could not generate object ({}, {})", FilePath, LineNumber, ScriptName, baseForm->GetFormID(), baseForm->GetName());
    }
    void Bind_GeneratedObject_BaseEditorID(const std::string& baseEditorId) {
        auto* form = LookupEditorID(baseEditorId);
        if (form) Bind_GeneratedObject(form);
    }
    void Bind_GeneratedObject_BaseFormID(RE::FormID baseFormID) {
        auto* form = LookupFormID(baseFormID);
        if (form) Bind_GeneratedObject(form);
    }
    void Bind_GeneratedQuest(std::string editorID = "") {
        auto* form = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESQuest>()->Create();
        if (!editorID.empty()) form->SetFormEditorID(editorID.c_str());
        Bind_Form(form);
    }
    void Bind_GeneratedSpell(RE::EffectSetting*) {
        auto* form = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>()->Create();
        // if (!editorID.empty()) form->SetFormEditorID(editorID.c_str());
        Bind_Form(form);
    }
    void Bind_GeneratedMagicEffect(RE::MagicSystem::CastingType castingType) {
        auto* form = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EffectSetting>()->Create();
        // if (!editorID.empty()) form->SetFormEditorID(editorID.c_str());
        form->data.castingType = castingType;

        Bind_Form(form);
        Bind_GeneratedSpell(form);
    }
    void Bind_FormID(RE::FormID formID) {
        auto* form = LookupFormID(formID);
        if (form) Bind_Form(form);
    }
    void Bind_EditorID(const std::string& editorID) {
        auto* form = LookupEditorID(editorID);
        if (form) Bind_Form(form);
    }
    bool IsUnderstoodScriptParentType(std::string parentTypeName) {
        LowerCase(parentTypeName);
        if (parentTypeName == "quest" || parentTypeName == "actor" || parentTypeName == "objectreference" || parentTypeName == "activemagiceffect") return true;
        return false;
    }
    void AutoBindBasedOnScriptExtends() {
        RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
        vm->GetScriptObjectType(ScriptName, typeInfo);
        std::string parentName;
        auto parent = typeInfo->parentTypeInfo;
        while (parent && !IsUnderstoodScriptParentType(parentName)) {
            parentName = parent->GetName();
            parent = parent->parentTypeInfo;
        }
        if (parentName.empty()) {
            logger::info("BIND ERROR [{}:{}] ({}) Cannot auto-bind to a script which does not `extends` anything", FilePath, LineNumber, ScriptName);
            return;
        }
        LowerCase(parentName);
        if (parentName == "quest")
            Bind_GeneratedQuest();
        else if (parentName == "actor")
            Bind_FormID(0x14);
        else if (parentName == "objectreference")
            Bind_GeneratedObject();
        else
            logger::info("BIND ERROR [{}:{}] ({}) No default BIND behavior available for script which `extends` {}", FilePath, LineNumber, ScriptName, parentName);
    }

    void ProcessBindingLine(std::string line) {
        if (line.empty()) return;
        trim(line);
        std::istringstream lineStream{line};
        lineStream >> ScriptName;
        if (ScriptName.empty() || ScriptName.starts_with('#') || ScriptName.starts_with("//")) return;
        if (!vm->TypeIsValid(ScriptName)) {
            logger::info("BIND ERROR [{}:{}] Script '{}' does not exist", FilePath, LineNumber, ScriptName);
            return;
        }
        logger::info("\"{}\"", line);
        std::string bindTarget;
        lineStream >> bindTarget;
        LowerCase(bindTarget);
        if (bindTarget.empty()) {
            AutoBindBasedOnScriptExtends();
            return;
        } else if (bindTarget.starts_with("0x"))
            Bind_FormID(std::stoi(bindTarget, 0, 16));
        else if (bindTarget == "$player")
            Bind_FormID(0x14);
        else if (bindTarget.starts_with("$quest("))
            Bind_GeneratedQuest(bindTarget.substr(7, bindTarget.size() - 8));
        else if (bindTarget == "$quest")
            Bind_GeneratedQuest();
        else if (bindTarget == "$object")
            Bind_GeneratedObject();
        else if (bindTarget.starts_with("$object(0x"))
            Bind_GeneratedObject_BaseFormID(std::stoi(bindTarget.substr(8, bindTarget.size() - 9), 0, 16));
        else if (bindTarget.starts_with("$object("))
            Bind_GeneratedObject_BaseEditorID(bindTarget.substr(8, bindTarget.size() - 9));
        else if (bindTarget.starts_with("$spelltype(")){
            std::string spellType = bindTarget.substr(10, bindTarget.size() - 11);
            if (spellType == "concentration")
                Bind_GeneratedMagicEffect(RE::MagicSystem::CastingType::kConcentration);
        }

        else
            Bind_EditorID(bindTarget);
    }

    void ProcessBindingFile() {
        logger::info("Reading Binding File: {}", FilePath);
        LineNumber = 1;
        std::string line;
        std::ifstream file{FilePath, std::ios::in};
        while (std::getline(file, line)) {
            LineNumber++;
            try {
                ProcessBindingLine(line);
            } catch (...) {
                logger::info("BIND ERROR [{}:{}]", FilePath, LineNumber);
            }
        }
        file.close();
    }

    void ProcessAllBindingFiles() {
        if (!std::filesystem::is_directory(BINDING_FILES_FOLDER_ROOT)) return;
        for (auto& file : std::filesystem::directory_iterator(BINDING_FILES_FOLDER_ROOT)) {
            FilePath = file.path().string();
            ProcessBindingFile();
        }
    }

    void OnGameStart() {
        vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        DefaultBaseFormForCreatingObjects = RE::TESForm::LookupByID(0xAEBF3);             // DwarvenFork
        LocationForPlacingObjects = RE::TESForm::LookupByID<RE::TESObjectREFR>(0xBBCD1);  // The chest in WEMerchantChests
        ProcessAllBindingFiles();
        for (auto binding : BindingLinesFromComments) ProcessBindingLine(binding);
    }

    bool InitJson() {
        if (std::filesystem::is_regular_file(DocstringJsonFilePath)) {
            std::ifstream jsonFileStream(DocstringJsonFilePath);
            std::stringstream jsonFileContent;
            jsonFileContent << jsonFileStream.rdbuf();
            Json::Reader reader;
            auto success = reader.parse(jsonFileContent.str(), DocstringJsonRoot);
            if (!success) {
                logger::error("Failed to parse json file required for Bind to parse script comments. Path: '{}'", DocstringJsonFilePath.string());
                return false;
            }
        } else {
            Json::Reader reader;
            reader.parse(DEFAULT_JSON, DocstringJsonRoot);
        }
        return true;
    }

    void SaveJson() {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::ofstream outputStream(DocstringJsonFilePath, std::ios::out);
        outputStream << Json::writeString(writer, DocstringJsonRoot);
    }

    void SearchForBindScriptDocStrings() {
        unsigned int scriptCount = 0;
        unsigned int unmodifiedScriptCount = 0;
        auto startTime = std::chrono::high_resolution_clock::now();
        ProcessingDocStrings = true;
        DocstringJsonFilePath = std::filesystem::current_path() / JSON_FILE_PATH;
        if (!std::filesystem::is_directory(DocstringJsonFilePath.parent_path())) std::filesystem::create_directory(DocstringJsonFilePath.parent_path());
        if (InitJson()) {
            auto& scriptBindComments = DocstringJsonRoot["scripts"];
            auto& mtimes = DocstringJsonRoot["mtimes"];
            auto scriptsFolder = std::filesystem::current_path() / "Data\\Scripts";

            for (auto& entry : std::filesystem::directory_iterator(scriptsFolder)) {
                if (!entry.is_regular_file()) continue;
                scriptCount++;

                auto scriptName = entry.path().filename().replace_extension().string();
                LowerCase(scriptName);
                try {
                    auto mtime = std::filesystem::last_write_time(entry.path()).time_since_epoch().count();
                    if (mtimes.isMember(scriptName) && mtimes[scriptName].asInt64() == mtime) {
                        unmodifiedScriptCount++;
                        if (scriptBindComments.isMember(scriptName)) {
                            for (auto bindComment : scriptBindComments[scriptName]) {
                                auto bindCommand = scriptName + " " + bindComment.asString().substr(5);
                                logger::info("{}", bindCommand);
                                BindingLinesFromComments.emplace_back(bindCommand);
                            }
                        }
                        continue;
                    }
                    mtimes[scriptName] = mtime;

                    auto pex = Pex::Binary();
                    auto reader = Pex::FileReader(entry.path().string());

                    try {
                        reader.read(pex);
                    } catch (...) {
                        logger::error("Champollion library failed to read .pex for script {}", scriptName);
                        continue;
                    }

                    auto docString = pex.getObjects().front().getDocString().asString();
                    if (!docString.empty()) {
                        std::string line;
                        std::istringstream commentString{docString};
                        std::atomic<bool> firstFoundBinding = true;
                        while (std::getline(commentString, line)) {
                            trim(line);
                            if (line.starts_with(BIND_COMMENT_PREFIX)) {
                                if (firstFoundBinding.exchange(false)) scriptBindComments[scriptName].clear();
                                auto bindCommand = scriptName + " " + line.substr(5);
                                BindingLinesFromComments.emplace_back(bindCommand);
                                scriptBindComments[scriptName].append(line);
                                logger::info("{}", bindCommand);
                            }
                        }
                    } else if (scriptBindComments.isMember(scriptName)) {
                        scriptBindComments.removeMember(scriptName);  // No longer has anything!
                    }
                } catch (...) {
                    logger::error("Error reading script {}", scriptName);
                }
            }
        }
        ProcessingDocStrings = false;
        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationInMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        logger::info("DocString Processing {} scripts ({} unmodified) took {}ms", scriptCount, unmodifiedScriptCount, durationInMs);
        SaveJson();
    }

    OnInit {
        spdlog::set_pattern("%v");
        std::thread t(SearchForBindScriptDocStrings);
        t.detach();
        GameStartedEventListener.callback = []() { OnGameStart(); };
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&GameStartedEventListener);
    }
}
