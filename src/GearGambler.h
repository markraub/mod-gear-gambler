#ifndef MOD_GEAR_GAMBLER_H
#define MOD_GEAR_GAMBLER_H

#include "ScriptMgr.h"
#include <vector>
#include <unordered_map>

class Player;

#define NPC_GEAR_GAMBLER         600100
#define NPC_TEXT_GEAR_GAMBLER    600100
#define GOSSIP_MENU_GEAR_GAMBLER 600100
#define GG_MAX_QUALITY           6   // 0-5 (Poor through Legendary)
#define GG_MAX_LEVEL             80

// Categories inside level brackets
enum GGCategory : uint8
{
    GG_CAT_WEAPONS   = 1,
    GG_CAT_ARMOR     = 2,
    GG_CAT_RESOURCES = 3,
    GG_CAT_OTHER     = 4,  // level-agnostic, lives outside brackets
    GG_CAT_MAX       = 5
};

enum GGTier : uint8
{
    GG_TIER_BRONZE   = 1,
    GG_TIER_SILVER   = 2,
    GG_TIER_GOLD     = 3,
    GG_TIER_PLATINUM = 4,
    GG_TIER_DIAMOND  = 5,
    GG_TIER_MAX      = 6
};

enum GGAction : uint32
{
    GG_ACTION_BACK_MAIN    = 50,
    GG_ACTION_OTHER_SELECT = 200,
};

static constexpr uint32 COPPER_PER_GOLD = 10000;

struct GGLootEntry
{
    uint32 itemEntry;
    float  weight;
    uint8  quality;         // cached from item_template
    int32  allowableClass;  // cached from item_template (-1 = all)
    uint32 requiredLevel;   // cached from item_template
};

class GearGamblerMgr
{
public:
    static GearGamblerMgr* Instance();

    void LoadConfig();
    void LoadLootTables();

    bool     IsEnabled()          const { return _enabled; }
    uint32   GetMinLevel()        const { return _minLevel; }
    uint32   GetSummonDuration()  const { return _summonDuration; }
    bool     ShouldAnnounce()     const { return _announceWins; }
    uint32   GetCommandGMLevel()  const { return _commandGMLevel; }
    uint32   GetSummonCooldown()  const { return _summonCooldown; }
    bool     AllowHigherBrackets() const { return _allowHigherBrackets; }

    uint8    NumBrackets()                    const;
    uint32   BracketMinLevel(uint8 bracket)   const;
    uint32   BracketMaxLevel(uint8 bracket)   const;

    uint32 GetTierPrice(uint8 tier)                        const; // gold
    uint32 GetScaledPriceCopper(uint8 tier, uint8 bracket) const; // copper
    uint32 GetOtherPriceCopper(uint8 tier)                 const; // copper

    // Loot rolling (bracket ignored for GG_CAT_OTHER)
    uint32 RollLoot(uint8 category, uint8 tier,
                    uint8 bracket, Player* player)          const;

    // Query helpers
    bool   HasLootInBracket(uint8 category, uint8 bracket,
                            Player* player)                 const;
    uint32 GetBracketEligibleCount(uint8 category, uint8 bracket,
                                   Player* player)          const;
    uint32 GetBracketTotalEligible(uint8 bracket, Player* player) const;
    bool   HasOtherLoot()                                   const;
    uint32 GetOtherLootCount()                              const;

    static const char* GetCategoryName(uint8 category);
    static const char* GetTierName(uint8 tier);
    static const char* GetTierColor(uint8 tier);

private:
    bool IsItemEligibleInBracket(const GGLootEntry& entry,
                                 uint8 category, uint8 bracket,
                                 Player* player) const;

    bool     _enabled             = true;
    uint32   _minLevel            = 1;
    uint32   _tierPrices[GG_TIER_MAX] = {};
    uint32   _summonDuration      = 300;
    bool     _announceWins        = true;
    uint32   _commandGMLevel      = 0;
    uint32   _summonCooldown      = 60;
    uint32   _bracketSize         = 5;
    bool     _allowHigherBrackets  = false;
    float    _otherPriceMultiplier = 3.0f;
    bool     _autoPopulate         = true;

    float _qualityWeights[GG_TIER_MAX][GG_MAX_QUALITY] = {};

    std::unordered_map<uint8, std::vector<GGLootEntry>> _lootTable;
};

#define sGearGamblerMgr GearGamblerMgr::Instance()

#endif
