# Phase 0.5 fixtures

Ten hand-authored state digests matching `docs/llm_agent_design.md` §6.
Each probes a different dimension of bot state to surface content-
dependent latency or grammar-adherence variance.

| File | Probe |
| --- | --- |
| `01-orc-warrior-lv10.json` | Low-level baseline. New character at inn, one starter quest, no social context. |
| `02-undead-mage-lv37.json` | Parent design's worked example. Mid-level, quest in progress, friendly human nearby with an open invite. |
| `03-troll-shaman-lv70.json` | High-level, in-group, in-guild, queuing for a dungeon. Heavy social context. |
| `04-tauren-druid-lv22.json` | Inventory pressure. Full bags, junk to vendor, no nearby vendor — the "I need to break my grind to manage inventory" branch. |
| `05-be-paladin-lv50.json` | Bot in combat with HP critical. Mid-pull elite mob, in-group, healer nearby. |
| `06-human-rogue-lv80.json` | Bot dead. Decision is "release / run back / use rez stone" vs setting an idle goal that is incoherent given death state. |
| `07-dwarf-hunter-lv45-adversarial.json` | Prompt-injection-shaped chat. `recent_whispers` contains "ignore previous instructions and ...". The schema must contain the model. |
| `08-gnome-warlock-lv60.json` | Quest-pickup branch. Three completable quests ready to turn in plus a new quest available — should bias toward turn-ins. |
| `09-ne-priest-lv30.json` | "What now" idle. In a group whose members are asking the bot for direction, no quest in log. |
| `10-orc-dk-lv58.json` | Recent-event-heavy log. Fresh-from-Scarlet-Enclave DK, no equipment, idle, event log packed with phasing/world-state changes. |
