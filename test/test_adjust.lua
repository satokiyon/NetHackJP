-- Modified by NetHackJP contributor @satokiyon; latest change date: 2026-11-02.
-- Tests for #adjust command (doorganize)

nh.parse_config("OPTIONS=fixinv")
nh.parse_config("OPTIONS=!timed_delay")
nh.debug_flags({ mongen = false, hunger = false, disable_pline = false })

local function meta(key)
    return string.char(0x80 | string.byte(key))
end

local function get_inv_map()
    local map = {}
    local count = 0
    local o = u.inventory
    while o do
        local tbl = o:totable()
        map[tbl.invlet] = tbl
        count = count + 1
        o = o:next()
    end
    return map, count
end

local function adjust(from_slot, to_slot, count)
    local keys = meta("a")
    if count then
        keys = keys .. tostring(count) .. from_slot
    else
        keys = keys .. from_slot
    end
    keys = keys .. to_slot
    nh.pushkey(keys)
    nh.doturn(true)
end

local function adjust_cancel(from_slot, count)
    local keys = meta("a")
    if count then
        keys = keys .. tostring(count) .. from_slot
    else
        keys = keys .. from_slot
    end
    keys = keys .. "\027" -- ESC
    nh.pushkey(keys)
    nh.doturn(true)
end

pline("=== test_adjust: Starting tests ===")

-- -------------------------------------------------------------
-- Case 1: 空きスロットへの単一アイテム移動 (Move to empty slot)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("rock"))
local inv = get_inv_map()
if not inv["a"] or inv["a"].otyp_name ~= "rock" then
    error("Case 1 Setup failed: rock not at slot 'a'")
end

adjust("a", "d")
inv = get_inv_map()
if inv["a"] ~= nil then
    error("Case 1 Failed: slot 'a' should be empty after move")
end
if not inv["d"] or inv["d"].otyp_name ~= "rock" or inv["d"].quan ~= 1 then
    error("Case 1 Failed: rock not moved to slot 'd'")
end
pline("Case 1 (Move to empty slot): OK")

-- -------------------------------------------------------------
-- Case 2: 異なるアイテム同士のスワップ (Swap two distinct items)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("rock"))
u.giveobj(obj.new("dagger"))
inv = get_inv_map()
if not inv["a"] or inv["a"].otyp_name ~= "rock" or not inv["b"] or inv["b"].otyp_name ~= "dagger" then
    error("Case 2 Setup failed: expected 'a'=rock, 'b'=dagger")
end

adjust("a", "b")
inv = get_inv_map()
if not inv["a"] or inv["a"].otyp_name ~= "dagger" then
    error("Case 2 Failed: slot 'a' should now have dagger")
end
if not inv["b"] or inv["b"].otyp_name ~= "rock" then
    error("Case 2 Failed: slot 'b' should now have rock")
end
pline("Case 2 (Swap two distinct items): OK")

-- -------------------------------------------------------------
-- Case 3: スタックの一部分割 (Split stack)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("20 darts"))
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 3 Setup failed: expected 20 darts at slot 'a'")
end

adjust("a", "f", 5)
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 15 then
    error("Case 3 Failed: slot 'a' should have 15 darts left, got " .. tostring(inv["a"] and inv["a"].quan))
end
if not inv["f"] or inv["f"].quan ~= 5 then
    error("Case 3 Failed: slot 'f' should have 5 darts, got " .. tostring(inv["f"] and inv["f"].quan))
end
pline("Case 3 (Split stack): OK")

-- -------------------------------------------------------------
-- Case 4: 同種スタックの統合 (Merge compatible stacks)
-- -------------------------------------------------------------
-- Now slot 'a' has 15 darts, slot 'f' has 5 darts.
adjust("f", "a")
inv = get_inv_map()
if inv["f"] ~= nil then
    error("Case 4 Failed: slot 'f' should be empty after merge")
end
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 4 Failed: slot 'a' should now have 20 darts after merge")
end
pline("Case 4 (Merge compatible stacks): OK")

-- -------------------------------------------------------------
-- Case 5: 同一スロット指定による集約 (Collect into same slot)
-- -------------------------------------------------------------
-- Split into 'a' and 'c'
adjust("a", "c", 8)
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 12 or not inv["c"] or inv["c"].quan ~= 8 then
    error("Case 5 Setup failed: split before collect failed")
end

-- Adjust 'a' to 'a' (collects compatible stacks into 'a')
adjust("a", "a")
inv = get_inv_map()
if inv["c"] ~= nil then
    error("Case 5 Failed: slot 'c' should be emptied into 'a' by collect")
end
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 5 Failed: slot 'a' should have 20 darts after collect")
end
pline("Case 5 (Collect into same slot): OK")

-- -------------------------------------------------------------
-- Case 6: 占有枠への分割と既存アイテムの追い出し (Split into occupied slot bumping occupant)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("20 darts")) -- slot 'a'
u.giveobj(obj.new("rock"))     -- slot 'b'
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 20 or not inv["b"] or inv["b"].otyp_name ~= "rock" then
    error("Case 6 Setup failed: expected 'a'=20 darts, 'b'=rock")
end

-- Split 6 darts from 'a' into occupied slot 'b'
adjust("a", "b", 6)
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 14 then
    error("Case 6 Failed: slot 'a' should have 14 darts")
end
if not inv["b"] or inv["b"].otyp_name ~= "dart" or inv["b"].quan ~= 6 then
    error("Case 6 Failed: slot 'b' should now have 6 darts")
end
-- Rock must have been bumped to an open slot (e.g. 'c')
local rock_found = false
for let, item in pairs(inv) do
    if item.otyp_name == "rock" and let ~= "a" and let ~= "b" then
        rock_found = true
        break
    end
end
if not rock_found then
    error("Case 6 Failed: rock was not bumped to an open slot")
end
pline("Case 6 (Split into occupied slot bumping occupant): OK")

-- -------------------------------------------------------------
-- Case 7: 名前付きアイテムの統合条件 (Named item merge behavior)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("10 arrows named silver")) -- 'a'
u.giveobj(obj.new("5 arrows"))               -- 'b'
inv = get_inv_map()
if not inv["a"] or not inv["b"] then
    error("Case 7 Setup failed: expected 'a' and 'b'")
end

-- Collect into unnamed slot 'b': unnamed 'from' into named 'candidate' should NOT merge
adjust("b", "b")
inv = get_inv_map()
if not inv["a"] or not inv["b"] then
    error("Case 7 Failed: unnamed slot 'b' collecting should not consume named candidate 'a'")
end

-- Collect into named slot 'a': named 'from' collects unnamed candidate 'b'
adjust("a", "a")
inv = get_inv_map()
if inv["b"] ~= nil then
    error("Case 7 Failed: unnamed candidate 'b' should have merged into named slot 'a'")
end
if not inv["a"] or inv["a"].quan ~= 15 or inv["a"].oname ~= "silver" then
    error("Case 7 Failed: slot 'a' should have 15 arrows named silver")
end
pline("Case 7 (Named item merge behavior): OK")

-- -------------------------------------------------------------
-- Case 8: 金貨スロット制約 (Gold constraint)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("rock"))
inv = get_inv_map()
if not inv["a"] then
    error("Case 8 Setup failed: rock not at 'a'")
end

-- Attempt to adjust rock to '$' slot
adjust("a", "$")
inv = get_inv_map()
if not inv["a"] or inv["a"].otyp_name ~= "rock" then
    error("Case 8 Failed: rock should still be at slot 'a' after rejected gold move")
end
if inv["$"] ~= nil then
    error("Case 8 Failed: non-gold item should never be placed in '$' slot")
end
pline("Case 8 (Gold slot constraint): OK")

-- -------------------------------------------------------------
-- Case 9: キャンセル操作 (Cancel / ESC restores state)
-- -------------------------------------------------------------
u.clear_inventory()
u.giveobj(obj.new("20 darts"))
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 9 Setup failed: expected 20 darts at 'a'")
end

-- Start splitting 5 darts, but press ESC at destination prompt
adjust_cancel("a", 5)
inv = get_inv_map()
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 9 Failed: ESC should abort split and preserve 20 darts at 'a'")
end
pline("Case 9 (Cancel / ESC handling): OK")

-- -------------------------------------------------------------
-- Case 10: インベントリ満杯時の分割失敗処理 (Knapsack full handling)
-- -------------------------------------------------------------
u.clear_inventory()
-- Fill slots 'a' through 'z' and 'A' through 'Z' (52 slots)
u.giveobj(obj.new("20 darts")) -- slot 'a'
for i = 2, 52 do
    u.giveobj(obj.new("rock named r" .. tostring(i)))
end

local total_items
inv, total_items = get_inv_map()
if total_items ~= 52 then
    error("Case 10 Setup failed: expected 52 inventory items, got " .. tostring(total_items))
end

-- Slot 'b' is occupied by a named rock. Attempt to split darts into 'b'.
-- Because 52 slots are already used, 'b' cannot be bumped into an open slot.
-- Split must be aborted with "荷物袋はいっぱいだ." without crashing.
adjust("a", "b", 5)
inv, total_items = get_inv_map()
if total_items ~= 52 then
    error("Case 10 Failed: inventory count changed after failed split (got " .. tostring(total_items) .. ")")
end
if not inv["a"] or inv["a"].quan ~= 20 then
    error("Case 10 Failed: slot 'a' should still have 20 darts after aborted split")
end
pline("Case 10 (Knapsack full handling): OK")

-- -------------------------------------------------------------
-- All tests completed successfully!
-- -------------------------------------------------------------
u.clear_inventory()
pline("test_adjust: ALL 10 TESTS PASSED SUCCESSFULLY!")

