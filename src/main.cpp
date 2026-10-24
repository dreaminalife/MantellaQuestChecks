// MantellaQuestChecks: answers Mantella's quest tag checks inside the game.
//
// Mantella turns the conditions of quest tags (e.g. `getstagedone CWObj 100=1`) into a list of checks.
// Mantella-Spell hands the list to this plugin, and asks for the answers at every conversation start
// and every player line. This plugin only answers each check with a plain number: comparing,
// combining and deciding what NPCs are told all happens in Mantella.
//
// Each check runs the game's own condition function (the one quest dialogue conditions use), so
// the answers match what the console shows.

namespace
{
	// Reserved answers, the same values as Mantella's src/quest_tags/conditions.py
	constexpr float kAnswerMissing = -9999901.0f;    // What the check is about doesn't exist in this game: not true
	constexpr float kAnswerNotLoaded = -9999902.0f;  // Exists, but isn't in memory right now (e.g. an NPC far away): unknown

	enum class Kind
	{
		kNone,
		kQuest,       // Editor ID
		kInt,         // A quest stage
		kActorValue,  // A skill name
		kActor,       // A placed NPC, or the player
		kNPC,         // An NPC's base form
		kFaction,
		kLocation,
		kKeyword,
		kGlobal,
		kSpell,
	};

	// How each function Mantella sends maps to a game condition function.
	// subjectIsThis: the condition runs on the subject, and the argument is its first parameter.
	// Otherwise the condition runs on the player, and subject / argument are its first / second parameter.
	struct FunctionSpec
	{
		std::string_view name;
		std::string_view command;
		bool             subjectIsThis;
		Kind             subject;
		Kind             argument;
	};

	constexpr FunctionSpec kFunctions[] = {
		{ "getstagedone"sv, "GetStageDone"sv, false, Kind::kQuest, Kind::kInt },
		{ "getquestcompleted"sv, "GetQuestCompleted"sv, false, Kind::kQuest, Kind::kNone },
		{ "getstage"sv, "GetStage"sv, false, Kind::kQuest, Kind::kNone },
		{ "getdead"sv, "GetDead"sv, true, Kind::kActor, Kind::kNone },
		{ "getdeadcount"sv, "GetDeadCount"sv, false, Kind::kNPC, Kind::kNone },
		{ "getav"sv, "GetBaseActorValue"sv, true, Kind::kActor, Kind::kActorValue },
		{ "getfactionrank"sv, "GetFactionRank"sv, true, Kind::kActor, Kind::kFaction },
		{ "getinfaction"sv, "GetInFaction"sv, true, Kind::kActor, Kind::kFaction },
		{ "getkeyworddataforlocation"sv, "GetKeywordDataForLocation"sv, false, Kind::kLocation, Kind::kKeyword },
		{ "getglobalvalue"sv, "GetGlobalValue"sv, false, Kind::kGlobal, Kind::kNone },
		{ "getrelationshiprank"sv, "GetRelationshipRank"sv, true, Kind::kActor, Kind::kActor },
		{ "ishostiletoactor"sv, "IsHostileToActor"sv, true, Kind::kActor, Kind::kActor },
		{ "hasspell"sv, "HasSpell"sv, true, Kind::kActor, Kind::kSpell },
	};

	// A subject or argument as Mantella sends it: "player", "File.esm|0D7505", a quest editor ID, a number or a skill name
	struct Value
	{
		bool          isPlayer{ false };
		std::string   file;
		RE::FormID    localID{ 0 };
		std::string   text;  // Lowercase quest editor ID, or the skill name
		std::uint32_t number{ 0 };
	};

	struct Check
	{
		const FunctionSpec* spec{ nullptr };  // Null when Mantella sent something this plugin can't read
		Value               subject;
		Value               argument;
	};

	std::mutex                                                 g_lock;
	std::string                                                g_version;
	std::vector<Check>                                         g_checks;
	std::unordered_map<std::string, RE::FormID>                g_questsByEditorID;  // Lowercase editor ID -> FormID, filled once the game's data is loaded
	std::unordered_map<std::string_view, RE::SCRIPT_FUNCTION*> g_commands;          // Static game data, filled once the game's data is loaded

	std::string ToLower(std::string_view a_text)
	{
		std::string result(a_text);
		std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	bool ParseValue(std::string_view a_text, Kind a_kind, Value& a_value)
	{
		switch (a_kind) {
		case Kind::kNone:
			return true;
		case Kind::kQuest:
		case Kind::kActorValue:
			a_value.text = ToLower(a_text);
			return !a_value.text.empty();
		case Kind::kInt:
			return std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_value.number).ec == std::errc{};
		default:
			{
				if (ToLower(a_text) == "player") {
					a_value.isPlayer = true;
					return true;
				}
				const auto separator = a_text.find('|');
				if (separator == std::string_view::npos) {
					return false;
				}
				a_value.file = std::string(a_text.substr(0, separator));
				const auto id = a_text.substr(separator + 1);
				return std::from_chars(id.data(), id.data() + id.size(), a_value.localID, 16).ec == std::errc{};
			}
		}
	}

	enum class Status
	{
		kOK,
		kMissing,
		kNotLoaded,
	};

	// Looks the form up fresh every time: NPCs who are only loaded nearby can be freed when the player leaves
	Status LookupForm(const Value& a_value, RE::TESForm*& a_form)
	{
		if (a_value.isPlayer) {
			a_form = RE::PlayerCharacter::GetSingleton();
			return a_form ? Status::kOK : Status::kNotLoaded;
		}
		auto*       dataHandler = RE::TESDataHandler::GetSingleton();
		const auto* file = dataHandler ? dataHandler->LookupModByName(a_value.file) : nullptr;
		if (!file || file->compileIndex == 0xFF) {
			return Status::kMissing;  // The mod isn't installed
		}
		a_form = dataHandler->LookupForm(a_value.localID, a_value.file);
		return a_form ? Status::kOK : Status::kNotLoaded;
	}

	// The value as the condition function takes it: a form pointer, or a number in the pointer's place
	Status Resolve(const Value& a_value, Kind a_kind, void*& a_param)
	{
		a_param = nullptr;
		switch (a_kind) {
		case Kind::kNone:
			return Status::kOK;
		case Kind::kInt:
			a_param = reinterpret_cast<void*>(static_cast<std::uintptr_t>(a_value.number));
			return Status::kOK;
		case Kind::kActorValue:
			{
				// Compared without case: Papyrus may hand strings over in another case than Mantella sent them
				for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::ActorValue::kTotal); ++i) {
					const char* name = RE::ActorValueList::GetActorValueName(static_cast<RE::ActorValue>(i));
					if (name && ToLower(name) == a_value.text) {
						a_param = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i));
						return Status::kOK;
					}
				}
				return Status::kMissing;
			}
		case Kind::kQuest:
			{
				const auto it = g_questsByEditorID.find(a_value.text);
				auto*      quest = it != g_questsByEditorID.end() ? RE::TESForm::LookupByID<RE::TESQuest>(it->second) : nullptr;
				if (!quest) {
					return Status::kMissing;
				}
				a_param = quest;
				return Status::kOK;
			}
		default:
			break;
		}

		RE::TESForm* form = nullptr;
		if (const auto status = LookupForm(a_value, form); status != Status::kOK) {
			return status;
		}
		// The wrong kind of form means a mistake in the condition: not true
		switch (a_kind) {
		case Kind::kActor:
			if (auto* actor = form->As<RE::Actor>()) {
				a_param = static_cast<RE::TESObjectREFR*>(actor);
			}
			break;
		case Kind::kNPC:
			a_param = form->As<RE::TESNPC>();
			break;
		case Kind::kFaction:
			a_param = form->As<RE::TESFaction>();
			break;
		case Kind::kLocation:
			a_param = form->As<RE::BGSLocation>();
			break;
		case Kind::kKeyword:
			a_param = form->As<RE::BGSKeyword>();
			break;
		case Kind::kGlobal:
			a_param = form->As<RE::TESGlobal>();
			break;
		case Kind::kSpell:
			a_param = form->As<RE::SpellItem>();
			break;
		default:
			break;
		}
		return a_param ? Status::kOK : Status::kMissing;
	}

	float Answer(const Check& a_check)
	{
		if (!a_check.spec) {
			return kAnswerMissing;
		}
		const auto command = g_commands.find(a_check.spec->name);
		if (command == g_commands.end() || !command->second || !command->second->conditionFunction) {
			return kAnswerMissing;
		}

		void*      subject = nullptr;
		void*      argument = nullptr;
		const auto subjectStatus = Resolve(a_check.subject, a_check.spec->subject, subject);
		const auto argumentStatus = Resolve(a_check.argument, a_check.spec->argument, argument);
		if (subjectStatus == Status::kMissing || argumentStatus == Status::kMissing) {
			return kAnswerMissing;
		}
		if (subjectStatus == Status::kNotLoaded || argumentStatus == Status::kNotLoaded) {
			return kAnswerNotLoaded;
		}

		RE::TESObjectREFR* thisObject = RE::PlayerCharacter::GetSingleton();
		void*              param1 = subject;
		void*              param2 = argument;
		if (a_check.spec->subjectIsThis) {
			thisObject = static_cast<RE::TESObjectREFR*>(subject);
			param1 = argument;
			param2 = nullptr;
		}
		if (!thisObject) {
			return kAnswerNotLoaded;
		}
		double result = 0.0;
		command->second->conditionFunction(thisObject, param1, param2, result);
		return static_cast<float>(result);
	}

	bool SetCheckList(RE::StaticFunctionTag*, std::string a_version, std::vector<std::string> a_functions, std::vector<std::string> a_subjects, std::vector<std::string> a_arguments)
	{
		if (a_subjects.size() != a_functions.size() || a_arguments.size() != a_functions.size()) {
			logs::warn("SetCheckList: {} functions, {} subjects, {} arguments - ignoring the list", a_functions.size(), a_subjects.size(), a_arguments.size());
			return false;
		}
		std::vector<Check> checks(a_functions.size());
		std::size_t        unreadable = 0;
		for (std::size_t i = 0; i < a_functions.size(); ++i) {
			const auto name = ToLower(a_functions[i]);
			const auto spec = std::ranges::find(kFunctions, std::string_view(name), &FunctionSpec::name);
			auto&      check = checks[i];
			if (spec == std::ranges::end(kFunctions) || !ParseValue(a_subjects[i], spec->subject, check.subject) || !ParseValue(a_arguments[i], spec->argument, check.argument)) {
				logs::warn("Can't read check {}: {} {} {}", i, a_functions[i], a_subjects[i], a_arguments[i]);
				++unreadable;
				continue;
			}
			check.spec = spec;
		}

		std::scoped_lock lock(g_lock);
		g_checks = std::move(checks);
		g_version = std::move(a_version);
		logs::info("Received check list {} with {} checks ({} unreadable)", g_version, g_checks.size(), unreadable);
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
		for (const auto& check : g_checks) {
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
		for (const auto& spec : kFunctions) {
			auto* command = RE::SCRIPT_FUNCTION::LocateScriptCommand(spec.command);
			if (!command || !command->conditionFunction) {
				logs::error("Condition function {} not found", spec.command);
			}
			g_commands[spec.name] = command;
		}

		std::scoped_lock lock(g_lock);
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
