#pragma once

// [CUE4P] EL item row names (DT_Items_Spirits / DT_Items_Passives) and display names for the overlay panel.
// `bit` is the stable bit index used in Vitals::passives_mask.

namespace lie::el_items {

struct Spirit {
    const char* id;
    const char* name;
    bool main; // main-attack spirit (Umbral Knight family)
};

inline constexpr Spirit kSpirits[] = {
    {"s5000", "黑色骑士 Umbral Knight", true},       {"s5020", "守护者西丽亚 Guardian Silva", true},
    {"s5030", "骑士长尤利乌斯 Captain Julius", true}, {"s5040", "黑色魔女伊莱恩 Dark Witch Eleine", true},
    {"s5050", "老战士格落特 Gerrod", true},           {"s5060", "深渊的守卫海尼尔 Hoenir", true},
    {"s5070", "狂骑士狼 Ulv the Mad Knight", true},   {"s5080", "异端者法腾 Faden the Heretic", true},
    {"s5010", "守护者西丽德 Guardian Siegrid", false}, {"s2002", "无头骑士 Headless Defender", false},
    {"s2012", "悬崖村的少年 Hamlet Youth", false},     {"s2022", "堕落的弓兵 Fallen Archer", false},
    {"s2032", "单眼的王之盾 One-Eyed Aegis", false},   {"s2052", "半秽变的罪犯 Incompetent Sinner", false},
    {"s2072", "隐藏的实验体 Hidden Test Subject", false}, {"s2082", "城堡里的姑娘 Castle Town Maiden", false},
    {"s2092", "守护者之长 Chief Guardian", false},     {"s2102", "西方商人 Western Merchant", false},
    {"s2112", "悬崖村村长 Hamlet Elder", false},       {"s2122", "菌之魔术师 Fungal Sorcerer", false},
    {"s2132", "花之魔女 Floral Sorceress", false},     {"s2162", "老守墓人 Elder Crypt Keeper", false},
    {"s2172", "禁区的战士 Verboten Champion", false},  {"s2182", "暗部的执行人 Dark Executioner", false},
    {"s2192", "异变的守卫 Fallen Sentinel", false},    {"s2232", "腐龙的孤儿 Forsaken Fellwyrm", false},
};

struct Passive {
    const char* id;
    int bit;
    const char* name;
};

inline constexpr Passive kPassives[] = {
    {"i_passive_maxhpup_LV1", 0, "脏污的银念珠 Soiled Prayer Beads"},
    {"i_passive_maxhpup_LV2", 1, "王之盾的徽章 Royal Aegis Crest"},
    {"i_passive_maxhpup_LV3", 2, "最大HP+ LV3"},
    {"i_passive_dmgcut_LV1", 3, "破损的音乐盒 Broken Music Box"},
    {"i_passive_dmgcut_LV2", 4, "破裂的魔石 Cracked Familiar Stone"},
    {"i_passive_dmgcut_LV3", 5, "雪之花手镯 Snowdrop Bracelet"},
    {"i_passive_dmgup", 6, "秽鬼之指 Blighted Appendage"},
    {"i_passive_dmgup_grounded", 7, "巨人的戒指 Giant's Ring"},
    {"i_passive_dmgup_grounded_LV2", 8, "地面伤害+ LV2"},
    {"i_passive_dmgup_airborne", 9, "古龙爪 Ancient Dragon Claw"},
    {"i_passive_dmgup_airborne_LV2", 10, "空中伤害+ LV2"},
    {"i_passive_dmgup_swimming", 11, "生锈的蓝色花卉装饰 Rusted Ornament"},
    {"i_passive_dmgup_maxhp", 12, "执行人的手套 Executioner's Gloves"},
    {"i_passive_stunstamina_damage_up", 13, "腐朽的王冠 Decayed Crown"},
    {"i_passive_regenHP_kill", 14, "击杀回HP Immortal's Crest"},
    {"i_passive_regenHP_attack", 15, "攻击回HP Aura's Ring"},
    {"i_passive_spirit_maxcast_count_up_LV1", 16, "风化项链 Weathered Necklace"},
    {"i_passive_spirit_maxcast_count_up_LV2", 17, "神圣羽毛 Sacred Feather"},
    {"i_passive_spirit_maxcast_count_up_LV3", 18, "技能次数+ LV3"},
    {"i_passive_recast_time_cut_LV1", 19, "陈旧怀表 Tarnished Watch"},
    {"i_passive_recast_time_cut_LV2", 20, "魔女耳环 Witch's Earring"},
    {"i_passive_recast_time_cut_LV3", 21, "技能冷却- LV3"},
    {"i_passive_heal_count_up_1", 22, "白巫女的雕像 Priestess Statue"},
    {"i_passive_heal_count_up_3", 23, "白巫女娃娃 Priestess' Doll"},
    {"i_passive_heal_count_up_2", 24, "白巫女的耳环 Priestess' Earrings"},
    {"i_passive_heal_power_up", 25, "纯净清泉的圣水 Holy Spring Water"},
    {"i_passive_shortheal", 26, "快速祈祷 Nymphilia's Ring"},
    {"i_passive_move_speed_up", 27, "魔线脚链 Spellbound Anklet"},
    {"i_passive_jump_height_up", 28, "七彩羽毛头饰 Vibrant Plume"},
    {"i_passive_swim_fast", 29, "污秽侵蚀的魔女之书 Witch's Book"},
    {"i_passive_expup_1", 30, "血染的丝带 Bloodstained Ribbon"},
    {"i_passive_expup_2", 31, "带有污秽的刀 Blightwreathed Blade"},
    {"i_passive_ignore_damage_area", 32, "异端者的面具 Heretic's Mask"},
    {"i_passive_stamina_up", 33, "耐力+"},
    {"i_passive_mp_restore_up_LV1", 34, "儿汝莉德的戒指 Eldred's Ring"},
    {"i_passive_maxmpup", 35, "莉柯莉斯的戒指 Ricorus' Ring"},
    {"i_passive_post_damage_invincibility", 36, "受击后无敌延长"},
    {"i_passive_flag_ending_c", 37, "光耀的守护宝具 Luminant Aegis Curio"},
    {"i_passive_override_skin_level", 38, "曾经失去的东西 Lost Heirloom"},
    {"i_passive_override_skin_level_max", 39, "污秽的幻影 Blighted Phantom"},
    {"i_passive_parry", 40, "芙莉蒂雅的戒指 Fretia's Ring"},
};

} // namespace lie::el_items
