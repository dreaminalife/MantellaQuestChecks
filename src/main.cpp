// MantellaQuestChecks: answers Mantella's quest tag checks inside the game.
//
// Mantella turns the conditions of quest tags (e.g. `getstagedone CWObj 100=1`) into a list of checks.
// Mantella-Spell hands the list to this plugin, and asks for the answers at every conversation start
// and every player line. This plugin only answers each check with a plain number: comparing,
// combining and deciding what NPCs are told all happens in Mantella.
//
// Each check runs one of the game's own condition functions (the ones quest dialogue conditions use),
// so the answers match what the console shows. Any condition function can be asked: the game's command
// table says what kind of value each one takes, and the values are read accordingly.

namespace
{
	// Reserved answers, the same values as Mantella's src/quest_tags/conditions.py
	constexpr float kAnswerMissing = -9999901.0f;     // What the check is about doesn't exist in this game: not true
	constexpr float kAnswerNotLoaded = -9999902.0f;   // Exists, but isn't in memory right now (e.g. an NPC far away): unknown
	constexpr float kAnswerUnreadable = -9999903.0f;  // The check can't be run (unknown function, wrong values): not true

	// The master files every Skyrim install has, by their load order prefix in FormIDs
	constexpr std::string_view kMasterFiles[] = { "Skyrim.esm"sv, "Update.esm"sv, "Dawnguard.esm"sv, "HearthFires.esm"sv, "Dragonborn.esm"sv };

	// Skill names, lowercase (values are lowercased: Papyrus may hand strings over in another case).
	// A fixed table: the game's own name lookup doesn't know names like OneHanded or Marksman
	struct Skill
	{
		std::string_view name;
		RE::ActorValue   actorValue;
	};

	constexpr Skill kSkills[] = {
		{ "onehanded"sv, RE::ActorValue::kOneHanded },
		{ "twohanded"sv, RE::ActorValue::kTwoHanded },
		{ "marksman"sv, RE::ActorValue::kArchery },
		{ "archery"sv, RE::ActorValue::kArchery },
		{ "block"sv, RE::ActorValue::kBlock },
		{ "smithing"sv, RE::ActorValue::kSmithing },
		{ "heavyarmor"sv, RE::ActorValue::kHeavyArmor },
		{ "lightarmor"sv, RE::ActorValue::kLightArmor },
		{ "pickpocket"sv, RE::ActorValue::kPickpocket },
		{ "lockpicking"sv, RE::ActorValue::kLockpicking },
		{ "sneak"sv, RE::ActorValue::kSneak },
		{ "alchemy"sv, RE::ActorValue::kAlchemy },
		{ "speechcraft"sv, RE::ActorValue::kSpeech },
		{ "speech"sv, RE::ActorValue::kSpeech },
		{ "alteration"sv, RE::ActorValue::kAlteration },
		{ "conjuration"sv, RE::ActorValue::kConjuration },
		{ "destruction"sv, RE::ActorValue::kDestruction },
		{ "illusion"sv, RE::ActorValue::kIllusion },
		{ "restoration"sv, RE::ActorValue::kRestoration },
		{ "enchanting"sv, RE::ActorValue::kEnchanting },
	};

	// How a value is read and handed to the condition function
	enum class Kind
	{
		kNumber,      // Passed as a number in the pointer's place
		kActorValue,  // A skill or other actor value name, passed as its number
		kQuest,       // Editor ID
		kReference,   // Anything placed in the world. Can leave memory, so not finding one means "not loaded"
		kActor,       // A placed NPC, or the player
		kForm,        // Any other form, always in memory once the game is loaded
	};

	template <class T>
	bool Is(RE::TESForm* a_form)
	{
		return a_form->As<T>() != nullptr;
	}

	bool IsInventoryObject(RE::TESForm* a_form) { return a_form->IsInventoryObject(); }
	bool IsAnyForm(RE::TESForm*) { return true; }
	bool IsOwner(RE::TESForm* a_form) { return Is<RE::TESFaction>(a_form) || Is<RE::TESNPC>(a_form); }

	struct ParamSpec
	{
		Kind kind;
		bool (*accepts)(RE::TESForm*){ nullptr };  // For kForm: whether a form is the kind the function takes
	};

	// The parameter types this plugin can read. Others (text, aliases, event data, ...) make the check unreadable
	std::optional<ParamSpec> GetParamSpec(RE::SCRIPT_PARAM_TYPE a_type)
	{
		using Type = RE::SCRIPT_PARAM_TYPE;
		switch (a_type) {
		case Type::kInt:
		case Type::kStage:
			return ParamSpec{ Kind::kNumber };
		case Type::kActorValue:
			return ParamSpec{ Kind::kActorValue };
		case Type::kQuest:
			return ParamSpec{ Kind::kQuest };
		case Type::kObjectRef:
		case Type::kContainerRef:
		case Type::kMapMarker:
			return ParamSpec{ Kind::kReference };
		case Type::kActor:
			return ParamSpec{ Kind::kActor };
		case Type::kInventoryObject:
			return ParamSpec{ Kind::kForm, IsInventoryObject };
		case Type::kSpellItem:
			return ParamSpec{ Kind::kForm, Is<RE::SpellItem> };
		case Type::kMagicItem:
			return ParamSpec{ Kind::kForm, Is<RE::MagicItem> };
		case Type::kCell:
			return ParamSpec{ Kind::kForm, Is<RE::TESObjectCELL> };
		case Type::kTopic:
			return ParamSpec{ Kind::kForm, Is<RE::TESTopic> };
		case Type::kRace:
			return ParamSpec{ Kind::kForm, Is<RE::TESRace> };
		case Type::kClass:
			return ParamSpec{ Kind::kForm, Is<RE::TESClass> };
		case Type::kFaction:
			return ParamSpec{ Kind::kForm, Is<RE::TESFaction> };
		case Type::kGlobal:
			return ParamSpec{ Kind::kForm, Is<RE::TESGlobal> };
		case Type::kActorBase:
		case Type::kNPC:
			return ParamSpec{ Kind::kForm, Is<RE::TESNPC> };
		case Type::kPackage:
			return ParamSpec{ Kind::kForm, Is<RE::TESPackage> };
		case Type::kMagicEffect:
			return ParamSpec{ Kind::kForm, Is<RE::EffectSetting> };
		case Type::kWeather:
			return ParamSpec{ Kind::kForm, Is<RE::TESWeather> };
		case Type::kFormList:
			return ParamSpec{ Kind::kForm, Is<RE::BGSListForm> };
		case Type::kPerk:
			return ParamSpec{ Kind::kForm, Is<RE::BGSPerk> };
		case Type::kVoiceType:
			return ParamSpec{ Kind::kForm, Is<RE::BGSVoiceType> };
		case Type::kIdleForm:
			return ParamSpec{ Kind::kForm, Is<RE::TESIdleForm> };
		case Type::kEquipType:
			return ParamSpec{ Kind::kForm, Is<RE::BGSEquipSlot> };
		case Type::kKeyword:
			return ParamSpec{ Kind::kForm, Is<RE::BGSKeyword> };
		case Type::kRefType:
			return ParamSpec{ Kind::kForm, Is<RE::BGSLocationRefType> };
		case Type::kLocation:
			return ParamSpec{ Kind::kForm, Is<RE::BGSLocation> };
		case Type::kShout:
			return ParamSpec{ Kind::kForm, Is<RE::TESShout> };
		case Type::kWordOfPower:
			return ParamSpec{ Kind::kForm, Is<RE::TESWordOfPower> };
		case Type::kAssociationType:
			return ParamSpec{ Kind::kForm, Is<RE::BGSAssociationType> };
		case Type::kOwner:
			return ParamSpec{ Kind::kForm, IsOwner };
		case Type::kForm:
			return ParamSpec{ Kind::kForm, IsAnyForm };
		default:
			return std::nullopt;
		}
	}

	// A reference or value as Mantella sends it, read for the kind of value the function takes
	struct Value
	{
		ParamSpec     spec{ Kind::kNumber };
		std::string   file;  // For forms: the file and the FormID within it
		RE::FormID    localID{ 0 };
		std::string   questEditorID;  // Lowercase
		std::uint32_t number{ 0 };    // For kNumber and kActorValue
	};

	struct Check
	{
		RE::SCRIPT_FUNCTION* command{ nullptr };  // Null when the check can't be run
		bool                 hasReference{ false };
		Value                reference;  // What the condition runs on; the player when there is none
		std::size_t          numParams{ 0 };
		Value                params[2];
		std::string          text;                // As Mantella sent it, for the log
		bool                 loggedWrongForm{ false };
	};

	std::mutex                                                   g_lock;
	std::string                                                  g_version;
	std::vector<Check>                                           g_checks;
	std::unordered_map<std::string, RE::FormID>                  g_questsByEditorID;  // Lowercase editor ID -> FormID, filled once the game's data is loaded
	std::unordered_map<std::string, RE::SCRIPT_FUNCTION*>        g_conditionFunctions;  // Lowercase long and short names, filled once the game's data is loaded

	std::string ToLower(std::string_view a_text)
	{
		std::string result(a_text);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	// "player", "File.esm|0D7505", or a FormID as the console shows it ("0D7505", "0201462A": the first two digits pick the master file)
	bool ParseForm(std::string_view a_text, Value& a_value)
	{
		if (ToLower(a_text) == "player") {
			a_value.file = kMasterFiles[0];
			a_value.localID = 0x14;
			return true;
		}
		if (const auto separator = a_text.find('|'); separator != std::string_view::npos) {
			a_value.file = std::string(a_text.substr(0, separator));
			const auto id = a_text.substr(separator + 1);
			const auto idEnd = id.data() + id.size();
			const auto [idPointer, idError] = std::from_chars(id.data(), idEnd, a_value.localID, 16);
			return !a_value.file.empty() && !id.empty() && idError == std::errc{} && idPointer == idEnd;
		}
		RE::FormID formID = 0;
		const auto end = a_text.data() + a_text.size();
		const auto [pointer, error] = std::from_chars(a_text.data(), end, formID, 16);
		const auto loadOrder = formID >> 24;
		if (a_text.empty() || a_text.size() > 8 || error != std::errc{} || pointer != end || loadOrder >= std::size(kMasterFiles)) {
			return false;  // A name, or a FormID from a mod: its first two digits depend on each player's load order
		}
		a_value.file = kMasterFiles[loadOrder];
		a_value.localID = formID & 0xFFFFFF;
		return true;
	}

	// Reads a value for its kind. Returns why it can't be read, or nothing when it can
	std::optional<std::string> ParseValue(std::string_view a_text, const ParamSpec& a_spec, Value& a_value)
	{
		a_value.spec = a_spec;
		switch (a_spec.kind) {
		case Kind::kNumber:
			{
				std::int32_t number = 0;
				const auto   end = a_text.data() + a_text.size();
				const auto [pointer, error] = std::from_chars(a_text.data(), end, number);
				if (error != std::errc{} || pointer != end) {
					return std::format("'{}' is not a number", a_text);
				}
				a_value.number = static_cast<std::uint32_t>(number);
				return std::nullopt;
			}
		case Kind::kActorValue:
			{
				const auto name = ToLower(a_text);
				if (const auto skill = std::ranges::find(kSkills, std::string_view(name), &Skill::name); skill != std::ranges::end(kSkills)) {
					a_value.number = static_cast<std::uint32_t>(skill->actorValue);
					return std::nullopt;
				}
				for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::ActorValue::kTotal); ++i) {
					const char* avName = RE::ActorValueList::GetActorValueName(static_cast<RE::ActorValue>(i));
					if (avName && ToLower(avName) == name) {
						a_value.number = i;
						return std::nullopt;
					}
				}
				return std::format("unknown actor value '{}'", a_text);
			}
		case Kind::kQuest:
			a_value.questEditorID = ToLower(a_text);
			return a_value.questEditorID.empty() ? std::optional<std::string>("no quest given") : std::nullopt;
		default:
			if (!ParseForm(a_text, a_value)) {
				return std::format("'{}' is not a FormID (names of factions, keywords etc. can't be looked up; mod forms are written Plugin.esp|00ABCD)", a_text);
			}
			return std::nullopt;
		}
	}

	enum class Status
	{
		kOK,
		kMissing,
		kNotLoaded,
		kWrongForm,  // A mistake in the condition: the FormID doesn't exist in its file, or is the wrong kind
	};

	// The value as the condition function takes it: a form pointer, or a number in the pointer's place.
	// Forms are looked up fresh every time: NPCs who are only loaded nearby can be freed when the player leaves
	Status Resolve(const Value& a_value, void*& a_param)
	{
		a_param = nullptr;
		switch (a_value.spec.kind) {
		case Kind::kNumber:
		case Kind::kActorValue:
			a_param = reinterpret_cast<void*>(static_cast<std::uintptr_t>(a_value.number));
			return Status::kOK;
		case Kind::kQuest:
			{
				const auto it = g_questsByEditorID.find(a_value.questEditorID);
				a_param = it != g_questsByEditorID.end() ? RE::TESForm::LookupByID<RE::TESQuest>(it->second) : nullptr;
				return a_param ? Status::kOK : Status::kMissing;  // No loaded file has a quest by that name
			}
		default:
			break;
		}

		auto*       dataHandler = RE::TESDataHandler::GetSingleton();
		const auto* file = dataHandler ? dataHandler->LookupModByName(a_value.file) : nullptr;
		if (!file || file->compileIndex == 0xFF) {
			return Status::kMissing;  // The mod isn't installed
		}
		auto* form = dataHandler->LookupForm(a_value.localID, a_value.file);
		const bool isPlaced = a_value.spec.kind == Kind::kReference || a_value.spec.kind == Kind::kActor;
		if (!form) {
			// Only placed things leave memory. Base NPCs, factions, places etc. are always loaded, so not finding one is a wrong ID
			return isPlaced ? Status::kNotLoaded : Status::kWrongForm;
		}
		switch (a_value.spec.kind) {
		case Kind::kActor:
			if (auto* actor = form->As<RE::Actor>()) {
				a_param = static_cast<RE::TESObjectREFR*>(actor);
			}
			break;
		case Kind::kReference:
			a_param = form->AsReference();
			break;
		default:
			if (a_value.spec.accepts(form)) {
				a_param = form;
			}
			break;
		}
		return a_param ? Status::kOK : Status::kWrongForm;
	}

	float Answer(Check& a_check)
	{
		if (!a_check.command) {
			return kAnswerUnreadable;
		}
		void*  values[3]{};  // The reference, then the parameters
		Status statuses[3]{ Status::kOK, Status::kOK, Status::kOK };
		if (a_check.hasReference) {
			statuses[0] = Resolve(a_check.reference, values[0]);
		}
		for (std::size_t i = 0; i < a_check.numParams; ++i) {
			statuses[i + 1] = Resolve(a_check.params[i], values[i + 1]);
		}
		if (std::ranges::find(statuses, Status::kWrongForm) != std::ranges::end(statuses)) {
			if (!a_check.loggedWrongForm) {
				a_check.loggedWrongForm = true;
				logs::warn("Can't run {}: a FormID doesn't exist in its file, or isn't the kind {} takes", a_check.text, a_check.command->functionName);
			}
			return kAnswerUnreadable;
		}
		if (std::ranges::find(statuses, Status::kMissing) != std::ranges::end(statuses)) {
			return kAnswerMissing;
		}
		if (std::ranges::find(statuses, Status::kNotLoaded) != std::ranges::end(statuses)) {
			return kAnswerNotLoaded;
		}

		auto* thisObject = a_check.hasReference ? static_cast<RE::TESObjectREFR*>(values[0]) : RE::PlayerCharacter::GetSingleton();
		if (!thisObject) {
			return kAnswerNotLoaded;
		}
		double result = 0.0;
		a_check.command->conditionFunction(thisObject, values[1], values[2], result);
		return static_cast<float>(result);
	}

	// Reads one check of the list. Returns why it can't be run, or nothing when it can
	std::optional<std::string> ReadCheck(std::string_view a_function, std::string_view a_reference, std::string_view a_param1, std::string_view a_param2, Check& a_check)
	{
		const auto it = g_conditionFunctions.find(ToLower(a_function));
		if (it == g_conditionFunctions.end()) {
			return "not one of the game's condition functions"s;
		}
		auto* command = it->second;

		if (!a_reference.empty()) {
			a_check.hasReference = true;
			if (auto error = ParseValue(a_reference, ParamSpec{ Kind::kReference }, a_check.reference)) {
				return error;
			}
		}

		const std::string_view given[] = { a_param1, a_param2 };
		a_check.numParams = a_param2.empty() ? (a_param1.empty() ? 0 : 1) : 2;
		if (a_param1.empty() && !a_param2.empty()) {
			return "a second value without a first"s;
		}
		std::size_t required = 0;
		for (std::size_t i = 0; i < command->numParams; ++i) {
			if (!command->params[i].optional) {
				++required;
			}
		}
		if (a_check.numParams < required || a_check.numParams > command->numParams) {
			return std::format("{} takes {} value(s), got {}", command->functionName, command->numParams, a_check.numParams);
		}
		for (std::size_t i = 0; i < a_check.numParams; ++i) {
			const auto type = command->params[i].paramType.get();
			const auto spec = GetParamSpec(type);
			if (!spec) {
				return std::format("{} value {} has a kind this plugin can't read (parameter type {})", command->functionName, i + 1, std::to_underlying(type));
			}
			if (auto error = ParseValue(given[i], *spec, a_check.params[i])) {
				return error;
			}
		}
		a_check.command = command;
		return std::nullopt;
	}

	bool SetCheckList(RE::StaticFunctionTag*, std::string a_version, std::vector<std::string> a_functions, std::vector<std::string> a_references, std::vector<std::string> a_params1, std::vector<std::string> a_params2)
	{
		const auto count = a_functions.size();
		if (a_references.size() != count || a_params1.size() != count || a_params2.size() != count) {
			logs::warn("SetCheckList: {} functions, {} references, {} first values, {} second values - ignoring the list", count, a_references.size(), a_params1.size(), a_params2.size());
			return false;
		}
		std::vector<Check> checks(count);
		std::size_t        unreadable = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const auto reference = a_references[i].empty() ? ""s : std::format("\"{}\".", a_references[i]);
			const auto text = std::format("check {}: {}{} {} {}", i, reference, a_functions[i], a_params1[i], a_params2[i]);
			if (auto error = ReadCheck(a_functions[i], a_references[i], a_params1[i], a_params2[i], checks[i])) {
				logs::warn("Can't run {} ({})", text, *error);
				checks[i] = Check{};
				++unreadable;
			}
			checks[i].text = text;
		}

		std::scoped_lock lock(g_lock);
		g_checks = std::move(checks);
		g_version = std::move(a_version);
		logs::info("Received check list {} with {} checks ({} can't be run)", g_version, g_checks.size(), unreadable);
		return true;
	}

	RE::BSFixedString GetListVersion(RE::StaticFunctionTag*)
	{
		std::scoped_lock lock(g_lock);
		return RE::BSFixedString(g_version.c_str());
	}

	// Not callable from tasklets: the game runs it on its main thread, where reading game state is safe
	std::vector<float> Evaluate(RE::StaticFunctionTag*)
	{
		std::scoped_lock   lock(g_lock);
		std::vector<float> answers;
		answers.reserve(g_checks.size());
		for (auto& check : g_checks) {
			answers.push_back(Answer(check));
		}
		return answers;
	}

	bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		constexpr auto script = "MantellaQuestChecks"sv;
		a_vm->RegisterFunction("SetCheckList"sv, script, SetCheckList);
		a_vm->RegisterFunction("GetListVersion"sv, script, GetListVersion, true);
		a_vm->RegisterFunction("Evaluate"sv, script, Evaluate);
		return true;
	}

	void OnDataLoaded()
	{
		std::scoped_lock lock(g_lock);
		auto*            commands = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
		// Long names first, so a short name that matches another command's long name never takes its place
		for (const bool longNames : { true, false }) {
			for (std::uint32_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd; ++i) {
				auto&       command = commands[i];
				const char* name = longNames ? command.functionName : command.shortName;
				if (command.conditionFunction && name && name[0]) {
					g_conditionFunctions.emplace(ToLower(name), &command);
				}
			}
		}
		logs::info("Found {} condition function names", g_conditionFunctions.size());

		if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
			for (const auto* quest : dataHandler->GetFormArray<RE::TESQuest>()) {
				const char* editorID = quest ? quest->GetFormEditorID() : nullptr;
				if (editorID && editorID[0]) {
					g_questsByEditorID.emplace(ToLower(editorID), quest->GetFormID());
				}
			}
		}
		logs::info("Found {} quests by editor ID", g_questsByEditorID.size());
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	SKSE::GetPapyrusInterface()->Register(RegisterFunctions);
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
		if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
			OnDataLoaded();
		}
	});

	logs::info("MantellaQuestChecks loaded");
	return true;
}
