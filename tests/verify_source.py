"""Static regression guards. These do not replace in-game testing."""
from pathlib import Path
import csv
import re

ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/auto_quest_start.cpp').read_text(encoding='utf-8')
for forbidden in ['GetProcAddress', 'BetterINQ_', 'ApplyFood', 'ApplyEquip', 'ApplyItem',
                  'FoodThread', 'BiqLateApplyThread', 'one_phase', 'equip_loadout',
                  'item_store', 'food_enable', 'use_biq', 'XInput']:
    assert forbidden not in s, forbidden
keys = re.findall(r'IniGet\(ini, "auto_quest_start", "([a-z_]+)"', s)
assert keys == ['enabled', 'save_slot', 'quest_id'], keys
for required in ['0x25E0D7', '0x1E7E890', '0x25FF03', '0x25FF0D', '0x25FF17',
                 '0x25FD83', '0x18D7130', '0x1B8F190', '0x261180', '0x22AD900',
                 'savedLobbyUi[0x258]', '0x18CA820', '0x0620C30', '0x1B0F5A0',
                 'createHub(net, 1, 0, 0)', 'c->rax = 0x1A',
                 'Wr32(gui + 0x28F0, 5)', 'c->rdx = 0',
                 'hook::InitArena(base, end, 0x1000)']:
    assert required in s, required
worker = s[s.index('DWORD WINAPI Worker'):]
assert worker.index('if (!g_cfg.enabled) return 0;') < worker.index('PatchAll(')
assert worker.index('if (!g_cfg.enabled) return 0;') < worker.index('InstallHookOnce(')
rows = list(csv.reader((ROOT / 'quest_ids.csv').open(encoding='utf-8-sig', newline='')))
assert len(rows) == 513
assert len({row[0] for row in rows[1:]}) == 512
print('PASS: three public settings, retained native startup calls, no obsolete bridge/loadout code, 512 quest IDs.')
