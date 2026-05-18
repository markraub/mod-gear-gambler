#include "GearGambler.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedGossip.h"
#include "World.h"
#include "WorldSession.h"
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// GearGamblerMgr -- singleton
// ---------------------------------------------------------------------------

GearGamblerMgr* GearGamblerMgr::Instance()
{
    static GearGamblerMgr instance;
    return &instance;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void GearGamblerMgr::LoadConfig()
{
    _enabled             = sConfigMgr->GetOption<bool>  ("GearGambler.Enable",             true);
    _minLevel            = sConfigMgr->GetOption<uint32>("GearGambler.MinLevel",            1);
    _summonDuration      = sConfigMgr->GetOption<uint32>("GearGambler.SummonDuration",      300);
    _announceWins        = sConfigMgr->GetOption<bool>  ("GearGambler.AnnounceWins",        true);
    _commandGMLevel      = sConfigMgr->GetOption<uint32>("GearGambler.CommandGMLevel",      0);
    _summonCooldown      = sConfigMgr->GetOption<uint32>("GearGambler.SummonCooldown",      60);
    _bracketSize         = sConfigMgr->GetOption<uint32>("GearGambler.BracketSize",         5);
    _allowHigherBrackets = sConfigMgr->GetOption<bool>  ("GearGambler.AllowHigherBrackets", false);
    _otherPriceMultiplier= sConfigMgr->GetOption<float> ("GearGambler.OtherPriceMultiplier",3.0f);
    _autoPopulate        = sConfigMgr->GetOption<bool>  ("GearGambler.AutoPopulate",        true);

    if (_bracketSize < 1)  _bracketSize = 1;
    if (_bracketSize > 80) _bracketSize = 80;
    if (_otherPriceMultiplier < 0.1f) _otherPriceMultiplier = 0.1f;

    _tierPrices[GG_TIER_BRONZE]   = sConfigMgr->GetOption<uint32>("GearGambler.BronzePrice",    50);
    _tierPrices[GG_TIER_SILVER]   = sConfigMgr->GetOption<uint32>("GearGambler.SilverPrice",   200);
    _tierPrices[GG_TIER_GOLD]     = sConfigMgr->GetOption<uint32>("GearGambler.GoldPrice",     500);
    _tierPrices[GG_TIER_PLATINUM] = sConfigMgr->GetOption<uint32>("GearGambler.PlatinumPrice", 1000);
    _tierPrices[GG_TIER_DIAMOND]  = sConfigMgr->GetOption<uint32>("GearGambler.DiamondPrice",  2500);

    static const char* tierNames[] = { "", "Bronze", "Silver", "Gold", "Platinum", "Diamond" };
    static const char* qualNames[] = { "Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary" };

    // Quality weights per tier.  Columns: Poor, Common, Uncommon, Rare, Epic, Legendary.
    // Weights are per-item; the roll pool = sum(count_Q * weight_Q) across all qualities.
    // Calibrated against actual item counts (pool ~35,800) to hit target probabilities:
    //   Bronze  : ~45% poor, ~45% common, ~10% uncommon, ~0.1% rare
    //   Silver  : ~20% common, ~79% uncommon, ~1% rare
    //   Gold    : ~20% uncommon, ~80% rare, ~0.01% epic
    //   Platinum: ~100% rare, ~0.05% epic  (1 in 2,000)
    //   Diamond : ~100% rare, ~0.1% epic   (1 in 1,000), ~0.02% legendary (1 in 5,000)
    static const float defaults[][6] = {
        {},
        //         Poor    Common  Uncommon   Rare    Epic    Legendary
        /* Bronze */ { 40.0f,  52.0f,   7.5f,  0.1f,   0.0f,   0.0f  },
        /* Silver */ {  0.0f,  24.0f,  60.0f,  1.0f,   0.0f,   0.0f  },
        /* Gold   */ {  0.0f,   0.0f,  25.0f,140.0f,   0.01f,  0.0f  },
        /* Plat   */ {  0.0f,   0.0f,   0.0f,175.0f,   0.05f,  0.0f  },
        /* Diamond*/ {  0.0f,   0.0f,   0.0f,175.0f,   0.1f,   4.75f },
    };

    for (uint8 t = GG_TIER_BRONZE; t < GG_TIER_MAX; ++t)
        for (uint8 q = 0; q < GG_MAX_QUALITY; ++q)
        {
            char key[128];
            snprintf(key, sizeof(key), "GearGambler.%s.%s", tierNames[t], qualNames[q]);
            _qualityWeights[t][q] = sConfigMgr->GetOption<float>(key, defaults[t][q]);
        }
}

// ---------------------------------------------------------------------------
// Loot table loading
// ---------------------------------------------------------------------------

void GearGamblerMgr::LoadLootTables()
{
    _lootTable.clear();

    struct Override { uint8 category; float weight; };
    std::unordered_map<uint32, Override> overrides;

    QueryResult ovr = WorldDatabase.Query(
        "SELECT category, item_entry, weight FROM mod_gear_gambler_loot");
    if (ovr)
    {
        do
        {
            Field* f         = ovr->Fetch();
            uint8  category  = f[0].Get<uint8>();
            uint32 itemEntry = f[1].Get<uint32>();
            float  weight    = f[2].Get<float>();

            if (category < GG_CAT_WEAPONS || category >= GG_CAT_MAX)
                continue;

            overrides[itemEntry] = { category, weight };
        }
        while (ovr->NextRow());
    }

    uint32 autoCount = 0;

    if (_autoPopulate)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT entry FROM item_template "
            "WHERE class IN (0, 1, 2, 4, 7, 15) "
            "AND Quality BETWEEN 0 AND 5 "
            "AND name != '' "
            "AND name NOT LIKE '%[PH]%' "   // placeholder items
            "AND name NOT LIKE '%[DEP]%' "  // deprecated items
            "AND name NOT LIKE '%test%' "   // test items (case-insensitive in MySQL)
            "AND name NOT LIKE 'QA %' "     // Blizzard QA items
            "AND name NOT LIKE 'QA\\_%' "   // QA_xxx items
            "AND name NOT LIKE '%UNUSED%' " // unused/deprecated
            "AND name NOT LIKE 'ZZ%' "      // ZZ-prefixed dev items
            "AND name NOT LIKE 'DO NOT%' "  // "Do Not Use" items
            "AND name NOT LIKE '%dummy%' "  // dummy items
            "AND name NOT LIKE '%delete%'");

        if (result)
        {
            do
            {
                uint32 itemEntry = result->Fetch()[0].Get<uint32>();

                if (overrides.count(itemEntry))
                    continue;

                const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemEntry);
                if (!proto) continue;

                uint8 category;
                switch (proto->Class)
                {
                    case 2:  category = GG_CAT_WEAPONS;    break;
                    case 4:  category = GG_CAT_ARMOR;      break;
                    case 0:
                    case 7:  category = GG_CAT_RESOURCES;  break;
                    case 1:
                    case 15: category = GG_CAT_OTHER;      break;
                    default: continue;
                }

                // Skip fishing poles (weapon subclass 20) — not useful loot
                if (proto->Class == 2 && proto->SubClass == 20)
                    continue;

                GGLootEntry entry;
                entry.itemEntry      = itemEntry;
                entry.weight         = 1.0f;
                entry.quality        = proto->Quality;
                entry.allowableClass = static_cast<int32>(proto->AllowableClass);
                entry.requiredLevel  = proto->RequiredLevel;
                entry.itemLevel      = proto->ItemLevel;
                entry.itemSubClass   = proto->SubClass;

                _lootTable[category].push_back(entry);
                ++autoCount;
            }
            while (result->NextRow());
        }

        LOG_INFO("module", ">> GearGambler: Auto-populated {} items from item_template (dev/test items excluded via name filter)",
                 autoCount);
    }

    uint32 addedOverrides = 0;
    uint32 blacklisted    = 0;

    for (auto const& [itemEntry, ov] : overrides)
    {
        if (ov.weight <= 0.0f)
        {
            ++blacklisted;
            continue;
        }

        const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto)
        {
            LOG_WARN("module", "GearGambler: override item {} not in item_template, skipped",
                     itemEntry);
            continue;
        }

        GGLootEntry entry;
        entry.itemEntry      = itemEntry;
        entry.weight         = ov.weight;
        entry.quality        = proto->Quality;
        entry.allowableClass = static_cast<int32>(proto->AllowableClass);
        entry.requiredLevel  = proto->RequiredLevel;
        entry.itemLevel      = proto->ItemLevel;
        entry.itemSubClass   = proto->SubClass;

        _lootTable[ov.category].push_back(entry);
        ++addedOverrides;
    }

    uint32 totalItems = 0;
    for (auto const& [cat, vec] : _lootTable) totalItems += vec.size();

    LOG_INFO("module",
             ">> GearGambler: {} total items ({} auto, {} overrides, {} blacklisted)",
             totalItems, autoCount, addedOverrides, blacklisted);
}

// ---------------------------------------------------------------------------
// Bracket math
// ---------------------------------------------------------------------------

uint8 GearGamblerMgr::NumBrackets() const
{
    return static_cast<uint8>((GG_MAX_LEVEL + _bracketSize - 1) / _bracketSize);
}

uint32 GearGamblerMgr::BracketMinLevel(uint8 bracket) const
{
    return static_cast<uint32>(bracket) * _bracketSize + 1;
}

uint32 GearGamblerMgr::BracketMaxLevel(uint8 bracket) const
{
    uint32 mx = (static_cast<uint32>(bracket) + 1) * _bracketSize;
    return (mx > GG_MAX_LEVEL) ? GG_MAX_LEVEL : mx;
}

// ---------------------------------------------------------------------------
// Pricing
// ---------------------------------------------------------------------------

uint32 GearGamblerMgr::GetTierPrice(uint8 tier) const
{
    return (tier < GG_TIER_MAX) ? _tierPrices[tier] : 0;
}

uint32 GearGamblerMgr::GetScaledPriceCopper(uint8 tier, uint8 bracket) const
{
    uint64 baseCopper = static_cast<uint64>(GetTierPrice(tier)) * COPPER_PER_GOLD;
    uint32 bMax       = BracketMaxLevel(bracket);
    uint64 scaled     = baseCopper * bMax * bMax
                      / (static_cast<uint64>(GG_MAX_LEVEL) * GG_MAX_LEVEL);
    return (scaled < 100) ? 100 : static_cast<uint32>(scaled);
}

uint32 GearGamblerMgr::GetOtherPriceCopper(uint8 tier) const
{
    uint64 baseCopper = static_cast<uint64>(GetTierPrice(tier)) * COPPER_PER_GOLD;
    uint64 price      = static_cast<uint64>(baseCopper * _otherPriceMultiplier);
    return (price < 100) ? 100 : static_cast<uint32>(price);
}

// ---------------------------------------------------------------------------
// Eligibility
// ---------------------------------------------------------------------------

// Returns the highest armor subclass (1=cloth,2=leather,3=mail,4=plate) a
// given class can equip.  Hunters and Shamans upgrade from leather to mail at 40.
static uint8 MaxArmorSubClassForPlayer(uint8 playerClass, uint32 playerLevel)
{
    switch (playerClass)
    {
        case 1: case 2: case 6:  return 4;                              // Warrior / Paladin / DK
        case 3: case 7:          return (playerLevel >= 40) ? 3 : 2;   // Hunter / Shaman
        case 4: case 11:         return 2;                              // Rogue / Druid
        default:                 return 1;                              // Priest / Mage / Warlock
    }
}

bool GearGamblerMgr::IsItemEligibleInBracket(const GGLootEntry& entry,
                                              uint8 category, uint8 bracket,
                                              Player* player) const
{
    uint32 bMin     = BracketMinLevel(bracket);
    uint32 bMax     = BracketMaxLevel(bracket);
    bool   isMaxBkt = (bMax == GG_MAX_LEVEL);

    // ---- Other (bags, mounts, misc) ----------------------------------------
    // Level-agnostic; still respect explicit class locks (e.g. paladin mount).
    if (category == GG_CAT_OTHER)
    {
        if (entry.allowableClass != -1)
        {
            uint32 mask = 1u << (player->getClass() - 1);
            if (!(static_cast<uint32>(entry.allowableClass) & mask))
                return false;
        }
        return true;
    }

    // ---- Resources: bracket by effective level ------------------------------
    // Trade goods usually have requiredLevel==0; fall back to itemLevel.
    if (category == GG_CAT_RESOURCES)
    {
        uint32 lvl = (entry.requiredLevel > 0) ? entry.requiredLevel : entry.itemLevel;
        if (lvl == 0)
            return (bracket == 0);
        // Level-80 bracket: allow a small underrun to capture WOTLK mats
        // whose itemLevel may land a few points below 76.
        if (isMaxBkt)
            return (lvl >= bMin - 5);
        return (lvl >= bMin && lvl <= bMax);
    }

    // ---- Weapons / Armor ----------------------------------------------------

    // 1. Level-bracket check
    if (entry.requiredLevel == 0)
    {
        if (bracket != 0) return false;
    }
    else if (isMaxBkt)
    {
        // Level-80 bracket shows only items that require exactly level 80.
        if (entry.requiredLevel != GG_MAX_LEVEL)
            return false;
    }
    else
    {
        if (entry.requiredLevel < bMin || entry.requiredLevel > bMax)
            return false;
    }

    // 2. Explicit class restriction from item_template
    if (entry.allowableClass != -1)
    {
        uint32 mask = 1u << (player->getClass() - 1);
        if (!(static_cast<uint32>(entry.allowableClass) & mask))
            return false;
    }

    // 3. Armor-type restriction: no plate for mages, no cloth for warriors, etc.
    if (category == GG_CAT_ARMOR)
    {
        uint8 sub = entry.itemSubClass;
        if (sub >= 1 && sub <= 4) // 1=cloth, 2=leather, 3=mail, 4=plate
        {
            uint8 maxArmor = MaxArmorSubClassForPlayer(
                player->getClass(), player->GetLevel());
            if (sub > maxArmor) return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Loot rolling
// ---------------------------------------------------------------------------

uint32 GearGamblerMgr::RollLoot(uint8 category, uint8 tier,
                                 uint8 bracket, Player* player) const
{
    auto it = _lootTable.find(category);
    if (it == _lootTable.end() || it->second.empty())
        return 0;

    struct Candidate { uint32 itemEntry; float ew; };
    std::vector<Candidate> pool;
    pool.reserve(it->second.size());

    for (const auto& entry : it->second)
    {
        if (!IsItemEligibleInBracket(entry, category, bracket, player))
            continue;
        if (entry.quality >= GG_MAX_QUALITY) continue;

        float qw = _qualityWeights[tier][entry.quality];
        if (qw <= 0.0f) continue;

        pool.push_back({ entry.itemEntry, entry.weight * qw });
    }

    if (pool.empty()) return 0;

    float total = 0.0f;
    for (const auto& c : pool) total += c.ew;

    float roll = frand(0.0f, total);
    float cum  = 0.0f;
    for (const auto& c : pool)
    {
        cum += c.ew;
        if (roll <= cum) return c.itemEntry;
    }
    return pool.back().itemEntry;
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

bool GearGamblerMgr::HasLootInBracket(uint8 category, uint8 bracket,
                                       Player* player) const
{
    auto it = _lootTable.find(category);
    if (it == _lootTable.end()) return false;
    for (const auto& e : it->second)
        if (IsItemEligibleInBracket(e, category, bracket, player))
            return true;
    return false;
}

uint32 GearGamblerMgr::GetBracketEligibleCount(uint8 category, uint8 bracket,
                                                Player* player) const
{
    auto it = _lootTable.find(category);
    if (it == _lootTable.end()) return 0;
    uint32 n = 0;
    for (const auto& e : it->second)
        if (IsItemEligibleInBracket(e, category, bracket, player))
            ++n;
    return n;
}

uint32 GearGamblerMgr::GetBracketTotalEligible(uint8 bracket, Player* player) const
{
    uint32 total = 0;
    for (uint8 c = GG_CAT_WEAPONS; c <= GG_CAT_RESOURCES; ++c)
        total += GetBracketEligibleCount(c, bracket, player);
    return total;
}

bool GearGamblerMgr::HasOtherLoot() const
{
    auto it = _lootTable.find(static_cast<uint8>(GG_CAT_OTHER));
    return it != _lootTable.end() && !it->second.empty();
}

uint32 GearGamblerMgr::GetOtherLootCount() const
{
    auto it = _lootTable.find(static_cast<uint8>(GG_CAT_OTHER));
    return (it != _lootTable.end()) ? static_cast<uint32>(it->second.size()) : 0;
}

// ---------------------------------------------------------------------------
// Name / color / format helpers
// ---------------------------------------------------------------------------

const char* GearGamblerMgr::GetCategoryName(uint8 cat)
{
    switch (cat)
    {
        case GG_CAT_WEAPONS:   return "Weapons";
        case GG_CAT_ARMOR:     return "Armor";
        case GG_CAT_RESOURCES: return "Resources";
        case GG_CAT_OTHER:     return "Other";
        default:               return "Unknown";
    }
}

const char* GearGamblerMgr::GetTierName(uint8 tier)
{
    switch (tier)
    {
        case GG_TIER_BRONZE:   return "Bronze";
        case GG_TIER_SILVER:   return "Silver";
        case GG_TIER_GOLD:     return "Gold";
        case GG_TIER_PLATINUM: return "Platinum";
        case GG_TIER_DIAMOND:  return "Diamond";
        default:               return "Unknown";
    }
}

const char* GearGamblerMgr::GetTierColor(uint8 tier)
{
    switch (tier)
    {
        case GG_TIER_BRONZE:   return "|cffCD7F32";
        case GG_TIER_SILVER:   return "|cffC0C0C0";
        case GG_TIER_GOLD:     return "|cffFFD700";
        case GG_TIER_PLATINUM: return "|cffE5E4E2";
        case GG_TIER_DIAMOND:  return "|cff00FFFF";
        default:               return "|cffffffff";
    }
}

static std::string BuildItemLink(const ItemTemplate* proto)
{
    if (!proto) return "[Unknown Item]";
    static const char* qc[] =
    { "9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","00ccff" };
    uint32 q = (proto->Quality <= 7) ? proto->Quality : 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             qc[q], proto->ItemId, proto->Name1.c_str());
    return buf;
}

static const char* TierFlavor(uint8 tier)
{
    switch (tier)
    {
        case GG_TIER_BRONZE:   return "A dusty crate -- miracles happen...";
        case GG_TIER_SILVER:   return "Decent odds for something useful";
        case GG_TIER_GOLD:     return "The sweet spot of risk and reward";
        case GG_TIER_PLATINUM: return "Premium quality, premium price";
        case GG_TIER_DIAMOND:  return "The best odds gold can buy";
        default:               return "";
    }
}

static std::string FormatPrice(uint32 copper)
{
    uint32 gold   = copper / 10000;
    uint32 silver = (copper % 10000) / 100;
    char buf[64];
    if (gold > 0 && silver > 0)
        snprintf(buf, sizeof(buf), "%ug %us", gold, silver);
    else if (gold > 0)
        snprintf(buf, sizeof(buf), "%ug", gold);
    else
        snprintf(buf, sizeof(buf), "%us", silver > 0 ? silver : 1);
    return buf;
}

// ---------------------------------------------------------------------------
// WorldScript
// ---------------------------------------------------------------------------

class GearGamblerWorldScript : public WorldScript
{
public:
    GearGamblerWorldScript() : WorldScript("GearGamblerWorldScript") {}
    void OnAfterConfigLoad(bool /*reload*/) override { sGearGamblerMgr->LoadConfig(); }
    void OnStartup() override
    {
        if (!sGearGamblerMgr->IsEnabled())
        { LOG_INFO("module", ">> GearGambler: Module disabled via config"); return; }
        sGearGamblerMgr->LoadLootTables();
        LOG_INFO("module", ">> GearGambler: Module loaded");
    }
};

// ---------------------------------------------------------------------------
// CreatureScript -- NPC gossip
// ---------------------------------------------------------------------------

class npc_gear_gambler : public CreatureScript
{
public:
    npc_gear_gambler() : CreatureScript("npc_gear_gambler") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!sGearGamblerMgr->IsEnabled())
        { CloseGossipMenuFor(player); return true; }

        if (player->GetLevel() < sGearGamblerMgr->GetMinLevel())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFF0000You must be at least level %u to use the Gear Gambler.|r",
                sGearGamblerMgr->GetMinLevel());
            CloseGossipMenuFor(player);
            return true;
        }
        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);
        uint8 numBrackets = sGearGamblerMgr->NumBrackets();

        if (action == GG_ACTION_BACK_MAIN)
            return ShowMainMenu(player, creature), true;

        if (action >= 5000 && action < 5000 + numBrackets)
            return ShowBracketCategoryMenu(player, creature,
                       static_cast<uint8>(action - 5000)), true;

        if (action >= 100 && action < 100 + numBrackets)
            return ShowBracketCategoryMenu(player, creature,
                       static_cast<uint8>(action - 100)), true;

        if (action == GG_ACTION_OTHER_SELECT)
            return ShowOtherTierMenu(player, creature), true;

        if (action >= 300 && action <= 1100)
        {
            uint8 bracket  = (action - 300) / 10;
            uint8 category = (action - 300) % 10;
            if (bracket < numBrackets && category >= GG_CAT_WEAPONS
                                      && category <= GG_CAT_RESOURCES)
                return ShowTierMenu(player, creature, category, bracket), true;
        }

        if (action >= 10000 && action <= 89999)
        {
            uint32 rem      = action - 10000;
            uint8  bracket  = rem / 1000;
            uint8  category = (rem % 1000) / 10;
            uint8  tier     = rem % 10;
            if (bracket < numBrackets && category >= GG_CAT_WEAPONS
                && category <= GG_CAT_RESOURCES
                && tier >= GG_TIER_BRONZE && tier < GG_TIER_MAX)
                return ExecutePurchase(player, creature, category, tier, bracket), true;
        }

        if (action >= 90001 && action <= 90005)
        {
            uint8 tier = action - 90000;
            if (tier >= GG_TIER_BRONZE && tier < GG_TIER_MAX)
                return ExecutePurchase(player, creature,
                           GG_CAT_OTHER, tier, 0), true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

private:
    void ShowMainMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        uint8  numBrackets = sGearGamblerMgr->NumBrackets();
        uint32 playerLevel = player->GetLevel();
        bool   any = false;

        for (uint8 b = 0; b < numBrackets; ++b)
        {
            uint32 bMax     = sGearGamblerMgr->BracketMaxLevel(b);
            bool   isMaxBkt = (bMax == GG_MAX_LEVEL);

            // Level-80 bracket: only available to level 80 players regardless
            // of AllowHigherBrackets (endgame gear is a different pool entirely).
            if (isMaxBkt)
            {
                if (playerLevel < GG_MAX_LEVEL) continue;
            }
            else if (!sGearGamblerMgr->AllowHigherBrackets() &&
                     sGearGamblerMgr->BracketMinLevel(b) > playerLevel)
                continue;   // bracket starts above the player's level — skip it

            uint32 total = sGearGamblerMgr->GetBracketTotalEligible(b, player);
            if (!total) continue;

            char label[128];
            if (isMaxBkt)
                snprintf(label, sizeof(label),
                         "|cffFFD700[Level 80]|r  Endgame gear  (%u items)", total);
            else
                snprintf(label, sizeof(label),
                         "|cffffffffLevels %u-%u|r   (%u items)",
                         sGearGamblerMgr->BracketMinLevel(b), bMax, total);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label,
                             GOSSIP_SENDER_MAIN, 100 + b);
            any = true;
        }

        if (sGearGamblerMgr->HasOtherLoot())
        {
            char label[128];
            snprintf(label, sizeof(label),
                     "|cff00ffffOther|r  -  Mounts, bags & surprises  (%u items)",
                     sGearGamblerMgr->GetOtherLootCount());
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, label,
                             GOSSIP_SENDER_MAIN, GG_ACTION_OTHER_SELECT);
            any = true;
        }

        if (!any)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffFF0000No items available for you right now.|r");
            CloseGossipMenuFor(player);
            return;
        }
        SendGossipMenuFor(player, NPC_TEXT_GEAR_GAMBLER, creature->GetGUID());
    }

    void ShowBracketCategoryMenu(Player* player, Creature* creature, uint8 bracket)
    {
        ClearGossipMenuFor(player);
        bool isMaxBkt = (sGearGamblerMgr->BracketMaxLevel(bracket) == GG_MAX_LEVEL);

        struct CatDef { uint8 cat; uint32 icon; const char* name; const char* desc; bool cls; };
        static const CatDef cats[] =
        {
            { GG_CAT_WEAPONS,   GOSSIP_ICON_BATTLE, "Weapons",   "Swords, axes, staves & more",  true  },
            { GG_CAT_ARMOR,     GOSSIP_ICON_TABARD, "Armor",     "Class-appropriate armor",       true  },
            { GG_CAT_RESOURCES, GOSSIP_ICON_VENDOR, "Resources", "Crafting materials & reagents", false },
        };
        for (const auto& c : cats)
        {
            uint32 cnt = sGearGamblerMgr->GetBracketEligibleCount(c.cat, bracket, player);
            if (!cnt) continue;
            char label[256];
            if (c.cls)
                snprintf(label, sizeof(label),
                         "|cffffffff%s|r  -  %s  (%u for your class)", c.name, c.desc, cnt);
            else
                snprintf(label, sizeof(label),
                         "|cffffffff%s|r  -  %s  (%u available)", c.name, c.desc, cnt);
            AddGossipItemFor(player, c.icon, label, GOSSIP_SENDER_MAIN,
                             300 + bracket * 10 + c.cat);
        }

        if (isMaxBkt && sGearGamblerMgr->HasOtherLoot())
        {
            // Surface the Other category from the main menu inside the 80 bracket too
            char label[128];
            snprintf(label, sizeof(label), "|cff00ffffOther|r  -  Mounts, bags & surprises  (%u items)",
                     sGearGamblerMgr->GetOtherLootCount());
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, label,
                             GOSSIP_SENDER_MAIN, GG_ACTION_OTHER_SELECT);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<< Back",
                         GOSSIP_SENDER_MAIN, GG_ACTION_BACK_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_GEAR_GAMBLER, creature->GetGUID());
    }

    void ShowTierMenu(Player* player, Creature* creature,
                      uint8 category, uint8 bracket)
    {
        ClearGossipMenuFor(player);
        const char* catName = GearGamblerMgr::GetCategoryName(category);

        for (uint8 t = GG_TIER_BRONZE; t < GG_TIER_MAX; ++t)
        {
            uint32 copper = sGearGamblerMgr->GetScaledPriceCopper(t, bracket);
            std::string priceStr = FormatPrice(copper);

            char label[256];
            snprintf(label, sizeof(label), "%s%s Box|r  -  %s  (%s)",
                     GearGamblerMgr::GetTierColor(t),
                     GearGamblerMgr::GetTierName(t),
                     TierFlavor(t), priceStr.c_str());

            char popup[256];
            snprintf(popup, sizeof(popup),
                     "Open a %s %s Box (Levels %u-%u)?\nCost: %s",
                     GearGamblerMgr::GetTierName(t), catName,
                     sGearGamblerMgr->BracketMinLevel(bracket),
                     sGearGamblerMgr->BracketMaxLevel(bracket),
                     priceStr.c_str());

            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, label,
                             GOSSIP_SENDER_MAIN,
                             10000 + bracket * 1000 + category * 10 + t,
                             popup, copper, false);
        }
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<< Back",
                         GOSSIP_SENDER_MAIN, 5000 + bracket);
        SendGossipMenuFor(player, NPC_TEXT_GEAR_GAMBLER, creature->GetGUID());
    }

    void ShowOtherTierMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        for (uint8 t = GG_TIER_BRONZE; t < GG_TIER_MAX; ++t)
        {
            uint32 copper = sGearGamblerMgr->GetOtherPriceCopper(t);
            std::string priceStr = FormatPrice(copper);

            char label[256];
            snprintf(label, sizeof(label), "%s%s Box|r  -  %s  (%s)",
                     GearGamblerMgr::GetTierColor(t),
                     GearGamblerMgr::GetTierName(t),
                     TierFlavor(t), priceStr.c_str());

            char popup[256];
            snprintf(popup, sizeof(popup),
                     "Open a %s Other Box?\nCost: %s",
                     GearGamblerMgr::GetTierName(t), priceStr.c_str());

            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, label,
                             GOSSIP_SENDER_MAIN, 90000 + t,
                             popup, copper, false);
        }
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<< Back",
                         GOSSIP_SENDER_MAIN, GG_ACTION_BACK_MAIN);
        SendGossipMenuFor(player, NPC_TEXT_GEAR_GAMBLER, creature->GetGUID());
    }

    void ExecutePurchase(Player* player, Creature* /*creature*/,
                         uint8 category, uint8 tier, uint8 bracket)
    {
        CloseGossipMenuFor(player);

        uint32 priceCopper = (category == GG_CAT_OTHER)
                           ? sGearGamblerMgr->GetOtherPriceCopper(tier)
                           : sGearGamblerMgr->GetScaledPriceCopper(tier, bracket);

        if (!player->HasEnoughMoney(priceCopper))
        {
            std::string ps = FormatPrice(priceCopper);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFF0000You need %s for this box!|r", ps.c_str());
            return;
        }

        uint32 itemEntry = sGearGamblerMgr->RollLoot(category, tier, bracket, player);
        const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!itemEntry || !proto)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffFF0000No eligible items found -- no gold taken.|r");
            return;
        }

        // Stackable resources: award a tier-scaled random quantity so players
        // actually get a useful amount of crafting materials.
        uint32 quantity = 1;
        if (category == GG_CAT_RESOURCES && proto->Stackable > 1)
        {
            static const uint32 qtyMin[GG_TIER_MAX] = { 0,  1,  3,  8, 15, 25 };
            static const uint32 qtyMax[GG_TIER_MAX] = { 0,  5, 15, 25, 40, 75 };
            quantity = urand(qtyMin[tier], qtyMax[tier]);
            if (quantity > proto->Stackable) quantity = proto->Stackable;
            if (quantity < 1)               quantity = 1;
        }

        ItemPosCountVec dest;
        uint32 noSpaceCount = 0;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, quantity,
                                    &noSpaceCount) != EQUIP_ERR_OK)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffFF0000Your bags are full!|r");
            return;
        }

        player->ModifyMoney(-static_cast<int32>(priceCopper));

        Item* item = player->StoreNewItem(dest, itemEntry, true);
        if (!item)
        {
            player->ModifyMoney(static_cast<int32>(priceCopper));
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffFF0000Failed to create item -- gold refunded.|r");
            return;
        }

        player->SendNewItem(item, quantity, true, false);
        std::string link = BuildItemLink(proto);

        if (category == GG_CAT_OTHER)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[Gear Gambler]|r You opened a %s%s Other Box|r and received: %s",
                GearGamblerMgr::GetTierColor(tier),
                GearGamblerMgr::GetTierName(tier), link.c_str());
        else if (quantity > 1)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[Gear Gambler]|r You opened a %s%s %s Box|r "
                "(Levels %u-%u) and received: %s x%u",
                GearGamblerMgr::GetTierColor(tier),
                GearGamblerMgr::GetTierName(tier),
                GearGamblerMgr::GetCategoryName(category),
                sGearGamblerMgr->BracketMinLevel(bracket),
                sGearGamblerMgr->BracketMaxLevel(bracket), link.c_str(), quantity);
        else
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[Gear Gambler]|r You opened a %s%s %s Box|r "
                "(Levels %u-%u) and received: %s",
                GearGamblerMgr::GetTierColor(tier),
                GearGamblerMgr::GetTierName(tier),
                GearGamblerMgr::GetCategoryName(category),
                sGearGamblerMgr->BracketMinLevel(bracket),
                sGearGamblerMgr->BracketMaxLevel(bracket), link.c_str());

        if (sGearGamblerMgr->ShouldAnnounce() && proto->Quality >= ITEM_QUALITY_EPIC)
        {
            LOG_INFO("module", "GearGambler: {} won [{}] from a {} {} Box!",
                     player->GetName(), proto->Name1,
                     GearGamblerMgr::GetTierName(tier),
                     GearGamblerMgr::GetCategoryName(category));
        }
    }
};

// ---------------------------------------------------------------------------
// CommandScript
// ---------------------------------------------------------------------------

using ChatCommandTable = Acore::ChatCommands::ChatCommandTable;

class gear_gambler_commandscript : public CommandScript
{
public:
    gear_gambler_commandscript() : CommandScript("gear_gambler_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable ggSubCmds =
        {
            { "summon", HandleSummon, SEC_PLAYER,        Acore::ChatCommands::Console::No  },
            { "reload", HandleReload, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes },
        };
        static ChatCommandTable table =
        {
            { "gg", ggSubCmds },
        };
        return table;
    }

    static bool HandleSummon(ChatHandler* handler, char const* /*args*/)
    {
        if (!sGearGamblerMgr->IsEnabled())
        { handler->SendSysMessage("|cffFF0000Gear Gambler is disabled.|r"); return true; }

        WorldSession* session = handler->GetSession();
        if (!session) { handler->SendSysMessage("In-game only."); return true; }

        if (session->GetSecurity() < sGearGamblerMgr->GetCommandGMLevel())
        { handler->SendSysMessage("|cffFF0000You lack permission.|r"); return true; }

        Player* player = session->GetPlayer();
        if (!player) return false;

        static std::unordered_map<uint32, time_t> cooldowns;
        time_t now  = time(nullptr);
        uint32 guid = player->GetGUID().GetCounter();

        auto it = cooldowns.find(guid);
        if (it != cooldowns.end())
        {
            int64 rem = static_cast<int64>(it->second)
                      + sGearGamblerMgr->GetSummonCooldown()
                      - static_cast<int64>(now);
            if (rem > 0)
            {
                handler->PSendSysMessage(
                    "|cffFF0000Wait %u seconds before summoning again.|r",
                    static_cast<uint32>(rem));
                return true;
            }
        }

        float x, y, z, o;
        player->GetPosition(x, y, z, o);
        x += 3.0f * std::cos(o);
        y += 3.0f * std::sin(o);

        uint32 despawnMs = sGearGamblerMgr->GetSummonDuration() * IN_MILLISECONDS;
        if (Creature* npc = player->SummonCreature(
                NPC_GEAR_GAMBLER, x, y, z, o + static_cast<float>(M_PI),
                TEMPSUMMON_TIMED_DESPAWN, despawnMs))
        {
            npc->SetFacingToObject(player);
            handler->PSendSysMessage(
                "|cffFFD700[Gear Gambler]|r Rizz Goldwheel has arrived! "
                "(despawns in %u seconds)", sGearGamblerMgr->GetSummonDuration());
            cooldowns[guid] = now;
        }
        else
            handler->SendSysMessage("|cffFF0000Failed to summon.|r");
        return true;
    }

    static bool HandleReload(ChatHandler* handler, char const* /*args*/)
    {
        sGearGamblerMgr->LoadConfig();
        sGearGamblerMgr->LoadLootTables();
        handler->SendSysMessage("|cffFFD700[Gear Gambler]|r Config & loot tables reloaded.");
        return true;
    }
};

// ---------------------------------------------------------------------------
// Registration -- called from the loader
// ---------------------------------------------------------------------------
void AddGearGamblerScripts()
{
    new GearGamblerWorldScript();
    new npc_gear_gambler();
    new gear_gambler_commandscript();
}
