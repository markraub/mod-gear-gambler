-- ============================================================
-- Gear Gambler Module  --  World Database Setup
-- ============================================================
-- Run against your world database (e.g. acore_world).
-- Safe to re-run: every section DELETEs before INSERTing.
--
-- Schema targets the liyunfan1223 / mod-playerbots AC fork.
-- ============================================================

SET @NPC_ENTRY := 600100;

-- -----------------------------------------------------------
-- 1. Creature template
-- -----------------------------------------------------------
DELETE FROM `creature_template` WHERE `entry` = @NPC_ENTRY;
INSERT INTO `creature_template` (
    `entry`, `name`, `subname`,
    `minlevel`, `maxlevel`, `faction`, `npcflag`,
    `speed_walk`, `speed_run`,
    `rank`, `unit_class`, `type`,
    `BaseAttackTime`, `RangeAttackTime`,
    `unit_flags`, `unit_flags2`, `flags_extra`,
    `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`,
    `ExperienceModifier`, `gossip_menu_id`,
    `AIName`, `ScriptName`
) VALUES (
    @NPC_ENTRY, 'Rizz Goldwheel', 'Gear Gambler',
    80, 80, 35, 1,
    1, 1.14286,
    0, 1, 7,
    2000, 2000,
    2, 2048, 2,
    100, 0, 1, 1,
    0, @NPC_ENTRY,
    '', 'npc_gear_gambler'
);

-- -----------------------------------------------------------
-- 2. Display model  (separate table in this AC fork)
-- -----------------------------------------------------------
-- DisplayScale 2.0 makes him tower over other NPCs.
-- CreatureDisplayID 7550 = goblin model.
DELETE FROM `creature_template_model` WHERE `CreatureID` = @NPC_ENTRY;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES (@NPC_ENTRY, 0, 7550, 2.0, 1.0);
--
-- TO CHANGE THE MODEL, update CreatureDisplayID:
--   UPDATE creature_template_model SET CreatureDisplayID = XXXXX WHERE CreatureID = 600100;
--
-- Try in-game with: .npc set model <displayId>
--   7550   Goblin (current)
--   20925  Ethereal
--   6760   Goblin in a tophat
--   7427   Goblin bruiser

-- -----------------------------------------------------------
-- 3. NPC greeting text
-- -----------------------------------------------------------
DELETE FROM `npc_text` WHERE `ID` = @NPC_ENTRY;
INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES (
    @NPC_ENTRY,
    'Well well, $c! Feeling lucky? Pick a level range, choose your category, and place your bet. Cheap boxes mostly turn up junk, but even a dusty crate can hide a legendary surprise...'
);

-- -----------------------------------------------------------
-- 4. Gossip menu link
-- -----------------------------------------------------------
DELETE FROM `gossip_menu` WHERE `MenuID` = @NPC_ENTRY;
INSERT INTO `gossip_menu` (`MenuID`, `TextID`) VALUES (@NPC_ENTRY, @NPC_ENTRY);

-- -----------------------------------------------------------
-- 5. Appearance -- equipment + aura
-- -----------------------------------------------------------
-- Thunderfury (main hand) + Warglaive of Azzinoth (off hand)
DELETE FROM `creature_equip_template` WHERE `CreatureID` = @NPC_ENTRY;
INSERT INTO `creature_equip_template` (`CreatureID`, `ID`, `ItemID1`, `ItemID2`, `ItemID3`)
VALUES (@NPC_ENTRY, 1, 19019, 32837, 0);

-- Visual aura: golden glow + visible from far away
DELETE FROM `creature_template_addon` WHERE `entry` = @NPC_ENTRY;
INSERT INTO `creature_template_addon` (`entry`, `path_id`, `mount`, `bytes1`, `bytes2`, `emote`, `visibilityDistanceType`, `auras`)
VALUES (@NPC_ENTRY, 0, 0, 0, 0, 0, 3, '18950');

-- -----------------------------------------------------------
-- 6. Permanent spawns  (all major cities)
-- -----------------------------------------------------------
DELETE FROM `creature` WHERE `id1` = @NPC_ENTRY;
INSERT INTO `creature` (
    `id1`, `map`, `zoneId`, `areaId`,
    `spawnMask`, `phaseMask`,
    `position_x`, `position_y`, `position_z`, `orientation`,
    `spawntimesecs`, `wander_distance`, `MovementType`
) VALUES
-- Dalaran
(@NPC_ENTRY, 571, 4395, 4613, 1, 1, 5807.77, 588.35, 660.94, 3.14, 300, 0, 0),
-- Shattrath
(@NPC_ENTRY, 530, 3703, 3703, 1, 1, -1838.16, 5301.87, -12.43, 5.88, 300, 0, 0),
-- Booty Bay
(@NPC_ENTRY, 0, 33, 35, 1, 1, -14438.72, 478.49, 15.12, 0.68, 300, 0, 0),
-- Stormwind
(@NPC_ENTRY, 0, 1519, 1519, 1, 1, -8830.87, 671.08, 97.90, 5.29, 300, 0, 0),
-- Ironforge
(@NPC_ENTRY, 0, 1537, 1537, 1, 1, -4917.87, -940.28, 501.56, 5.41, 300, 0, 0),
-- Darnassus
(@NPC_ENTRY, 1, 1657, 1657, 1, 1, 9961.17, 2055.72, 1328.81, 1.59, 300, 0, 0),
-- Exodar
(@NPC_ENTRY, 530, 3557, 3557, 1, 1, -3965.69, -11653.59, -138.84, 0.85, 300, 0, 0),
-- Orgrimmar
(@NPC_ENTRY, 1, 1637, 1637, 1, 1, 1633.68, -4439.35, 17.71, 2.18, 300, 0, 0),
-- Thunder Bluff
(@NPC_ENTRY, 1, 1638, 1638, 1, 1, -1278.45, 122.97, 131.05, 5.14, 300, 0, 0),
-- Undercity
(@NPC_ENTRY, 0, 1497, 1497, 1, 1, 1596.55, 237.71, -52.15, 0.56, 300, 0, 0),
-- Silvermoon
(@NPC_ENTRY, 530, 3487, 3487, 1, 1, 9484.09, -7294.22, 14.30, 6.17, 300, 0, 0);

-- -----------------------------------------------------------
-- 7. Override / blacklist table  (OPTIONAL)
-- -----------------------------------------------------------
DROP TABLE IF EXISTS `mod_gear_gambler_loot`;
CREATE TABLE `mod_gear_gambler_loot` (
    `id`         INT UNSIGNED       NOT NULL AUTO_INCREMENT,
    `category`   TINYINT UNSIGNED   NOT NULL COMMENT '1=Weapons 2=Armor 3=Resources 4=Other',
    `item_entry` MEDIUMINT UNSIGNED NOT NULL COMMENT 'item_template.entry',
    `weight`     FLOAT              NOT NULL DEFAULT 1.0 COMMENT 'Drop weight (0 or negative = blacklist)',
    PRIMARY KEY (`id`),
    INDEX `idx_category` (`category`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Gear Gambler overrides & blacklist';
