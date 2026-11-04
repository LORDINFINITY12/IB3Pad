# Changelog

## 0.6.0-beta7.1

- Focus outline: reverted to the exact beta 6 code. The beta 7 "rebuild" logic is gone.
- The game window is left untouched again, as in beta 6 and earlier. Beta 7 swapped in its own window procedure to run commands; commands now run on the game thread from a message hook instead, with the same direct calls.
- Supplies: keys, ingredients and the other categories that show one centred button (the game's UseCenteredButton) now work. A and Y press that button, and the cursor and outline park on it.
- Fixed crashes on scene changes: a direct command no longer runs while the game is tearing down or loading a level (the old player or HUD is destroyed, the scene changed under 0.6 s ago, or the press waited in the queue because the game was loading).

## 0.6.0-beta7

- Controller buttons now call the game's own command functions directly (SwordPC.ButtonDodgeLeft, SwordPC.SlashLeft, SwordHudMain.ActivateMagicMode, SwordPC.ScrollUp and the rest) through the engine's ProcessEvent, on the game's thread. No keyboard keys and no key bindings are involved any more, so nothing can collide with keys the game or Windows handle themselves (the beta 6 F11 fullscreen toggle cannot happen) and launcher keybinds never matter. IB3Pad no longer adds anything to SwordInput.ini for bindings; it only removes session bindings an older version left behind.
- Until the game's objects are found in the first seconds after launch, an action falls back to the port's default key for it.
- Focus outline: a rebuild-on-window-change step was added (reverted in beta 7.1).

## 0.6.0-beta6.2

- Magic (RT) no longer uses F11 as its session key: the game handles F11 itself as its fullscreen toggle, so tapping RT could drop the game out of fullscreen. Magic now uses Scroll Lock, which the game neither handles nor binds.

## 0.6.0-beta6.1

- Removed the beta 5 controls for the player-info / potion pop-up: the game shows that pop-up by itself while exploring, and the profile took over the left stick and parked the cursor on the potion button. It keeps the plain 0.5 controls again; the Supplies screen keeps its controller controls.

## 0.6.0-beta6

- Keyboard-independent: every controller action is now one of the game's own commands, never a keyboard key, so changing keybinds in the launcher can no longer break the controller. Dodges, block, super, magic and the three magic slots, auto-stab, sword clash, pause, quit prompt, fast-forward, boss info, popup OK and all menu scrolling (including what the mouse wheel did) are bound, for the session only, to keys the game leaves free (F1, F4 to F8, Scroll Lock, numpad + . and Pause for combat; [ ] ; ' , . / = for menus), next to the 12 slash keys as before, and removed on exit. The commands are copied from the port's own DefaultInput.ini.
- `ib3pad.ini` now maps buttons to action names (DodgeLeft, Block, Super, ScrollUp...) instead of keys. Plain key names still work for custom setups.
- If the session bindings cannot be written (for example a read-only settings file), each action falls back to the port's default key, so nothing goes dead.
- Includes beta 5 (Blacksmith, progress lists, Cauldron / Fusion one-press).

## 0.6.0-beta5

- Blacksmith: D-pad up/down steps through the item list (the game has no scroll command there, so IB3Pad drags the list by exactly one item, along the list's own direction), left/right uses the item-type arrows, A presses UPGRADE (the cursor is parked on it), LB / RB switch Siris / Isa.
- Forge, Fusion and Cauldron progress lists: the outline moves between each slot's main bar (UPGRADE MASTERED ITEM / FUSE NEW GEM / COOK NEW POTION / UNLOCK SLOT) and its FINISH NOW, read from the row's own buttons; the list scrolls at the ends. A clicks.
- Cauldron and Fusion Chamber: no more dragging. A on an ingredient or gem moves it straight into the next empty cauldron / fusion slot and puts the cursor back, so you can press again; A on a filled slot takes it back out. Y = COOK / FUSE, X = the cauldron's own cancel / take the last gem out of fusion. Which slots are cells and which are cauldron / fusion slots, and which are filled, is read from the game's own slot data.
- Forge / Fusion / Cauldron result screens get the outline and X snap.
- The potion pop-up you open while exploring (player info with the potion button) gets controller controls: the cursor starts on the potion button so A drinks, the D-pad reaches its other buttons, X snaps.

## 0.6.0-beta4

- Hub fixed: the folded gear menu was being taken for an open one (it keeps some opacity when folded), which took over the hub. It now counts as open only by the game's own rule: opacity above zero and the gear button showing its "open" icon. Items, Supplies, Skills, Customize, Goals and the gear are back on the D-pad, and LB / RB switch Siris / Isa again.
- Skills: Y (or Start) accepts the points you added, B cancels them (before, leaving the screen kept them), X resets all points.

## 0.6.0-beta3

- A is a plain click at the cursor again on every screen, except Items / Supplies (buy, equip, use) and the Gem Bag (pick up / drop). It never jumps the cursor to a button.
- Nothing snaps the cursor when a screen opens any more (only Items / Supplies park it on BUY / EQUIP / USE). X snaps the outline and cursor to the nearest button.
- Reward pop-ups while exploring (e.g. after picking up a coin bag) keep the normal controls.
- The D-pad starts from the button under the cursor, so pressing down from CUSTOMIZE goes to GOALS.
- Hub: right on the D-pad reaches the gear. The open gear menu (Options, Help, Hideout...) is navigable; it is detected the way the game does it (its opacity).
- Customize: the side bar's sub-menus are navigable row by row and scroll by dragging the list.
- Goals: moving to a row puts the cursor on its COLLECT label, so A collects; empty spacer rows are skipped.
- Gem Bag: the item's socket rows are pick-up and drop spots too (outline on the gem icon), so gems can be taken out of an item and put in.
- R3 stays a holdable click in menus.

## 0.6.0-beta2

- Rebuilt around screen-specific controls instead of one generic outline. Screens without their own controls keep 0.5 behaviour exactly.
- Hub: fixed the outline landing on empty space (the folded gear menu sits on top of the hub in the game's screen stack and was being used instead of the hub). Up/down picks Items, Supplies, Skills, Customize, Goals; LB and RB both switch Siris / Isa; Y opens the gear menu.
- Items: D-pad scrolls items and changes item type as before; the cursor sits on BUY / EQUIP and A presses it; LB / RB = ALL / INVENTORY / STORE; LT = items, RT = Gem Bag.
- Gem Bag: the outline moves between the gem grid cells and the item's sockets (the game's own slot rectangles); A picks a gem up and carries it with no button held, A drops it; B puts it back; Y sells it.
- Supplies: A = use, Y = buy, D-pad scrolls.
- Skills: up/down goes to each stat's number, left/right walks that row's skills; A adds a point or picks a skill; Start confirms, Y cancels, X resets points.
- Goals: up/down picks a goal row (scrolling at the ends), A presses that row's COLLECT.
- Menu positions now follow the game's own rule (owner screen or anchor object plus offset), which fixes Customize's icons being placed on the wrong side.

## 0.6.0-beta

- Console-style menus. In every menu the D-pad or left stick moves a gold outline between the screen's own buttons, tabs and lists (read live from the game), and the cursor sits on the outlined one, so A presses exactly that. Holding a direction repeats.
- Each new screen starts on its main button (CONTINUE, ITEMS, OK...), or on what you last chose there. Popups start on No / Cancel; Start says OK / Yes (the game's own accept command), B says No / Close.
- A focused list scrolls with up/down (left/right for a sideways list); the other direction leaves it.
- LB / RB switch the screen's tabs (Buy / Sell, Skills / Stats, ALL / INVENTORY / STORE...), or Siris / Isa on screens without tabs (hub, Blacksmith).
- View opens the screen's ? help in menus; it still fast-forwards cutscenes.
- The 0.5.4 results-screen code is folded into this.
- With DebugInput=1 or LogScreens=1, every screen's buttons are written to ib3pad.log when it opens, to tune per-screen controls.

## 0.5.4

- R3 while exploring swings the camera back to centre, using the game's own camera glide (it writes the camera's target look angle back to zero). R3 keeps its click in menus and fights.
- Fight results screen (CONTINUE / INVENTORY / EXIT ARENA): the cursor starts on CONTINUE, the D-pad moves between the buttons and A presses.
- Logs which thread the game polls the controller on (groundwork for 0.6).

## 0.5.3

- Fixed the left stick falling back to mouse swipes (and lagging) in fights after a level change. The game's object table grows and moves when a level loads; IB3Pad now re-reads it live, so it finds every new fight HUD within a fraction of a second.
- When the game screen is unknown (loading, HUD not found yet) the left stick now uses the direct key slashes. The mouse swipe is used only when the game positively reports exploring.

## 0.5.2

- The cursor can no longer escape the game window: the game's own cursor-lock calls are kept on the window, and the lock is checked every 2 ms (was 250 ms).

## 0.5.1

- B now presses the current screen's own on-screen back arrow (found in the game's menu data), so it goes back exactly one layer, e.g. from a forge upgrade back to the weapon list, instead of closing the menu.

## 0.5.0

- B goes back one step in every menu using the game's own back button, and closes popups with their own Close/No. Outside menus it is still dodge right.
- IB3Pad reads the game's open-menu stack (read-only) and logs which screen is on top, the basis for per-screen controls.

## 0.4.3

- The finishing bonus after a boss falls counts as a fight, so the left stick keeps instant slashes instead of switching to the swipe.

## 0.4.2

- Fixed fight detection failing on the real game (the property layout is now matched exactly against the HUD's known structure), so the left stick switches to instant slashes in fights.

## 0.4.1

- Session changes to SwordInput.ini are now always undone when the game closes, including the game's abrupt shutdown on quit and crashes (a small cleanup helper waits for the game to exit).

## 0.4.0

- Automatic fight detection: IB3Pad reads the game's current screen (its HUD mode) from memory, read-only. No buttons or toggles.
- Left stick: click-and-drag swipe outside fights (fast camera turning while exploring, now with the inverted camera), instant key slashes in fights.
- All memory scanning runs on its own thread, so controller input never stalls.

## 0.3.2

- Left stick looks around while exploring again (it still slashes on flicks in fights). LeftStickLook / LeftStickSpeed in [Cursor].

## 0.3.1

- Right stick now sends real (relative) mouse movement: one-to-one camera control while exploring the hub and levels, normal cursor in menus, and no stopping at the window edge.

## 0.3.0

- Instant slashes: each of the game's 12 slash directions is a direct key press instead of an emulated swipe (about 100 ms faster, no queueing). The keys are bound only while the game runs.
- Rolling flicks: swing the stick to a new direction to chain slashes without returning to the centre (RollAngle).
- D-pad slashes no longer need launcher keybinds.
- Camera and binding changes restore the user's SwordInput.ini byte-for-byte on exit, and after a crash on the next launch.

## 0.2.0

- One layout for fights and menus; the R3 mode toggle is gone (still available as an advanced option).
- LB or R3 clicks; hold and move the right stick to drag-scroll.
- Y now auto-stabs and confirms popups. Start pauses in fights and goes back in menus.
- LT triggers Super through the launcher's Super keybind (Q); no launcher setup needed.
- Cursor kept inside the game window while focused (ConfineCursor).
- Buttons can send several inputs at once (F+Enter).
- D-pad scrolls menus and slashes in fights (slash keys bound in the launcher).
- A selects (click) and blocks.
- Wheel actions (WheelUp/Down/Left/Right) that repeat while held.
- L3 + R3 together opens the quit prompt (L3R3Together).
- Magic: RT tap opens magic mode, RT + X/Y/B casts a slot directly; triggers no longer flicker near the threshold.
- Camera look inverted on both axes while the mod is installed ([Camera]); applied at launch, restored at exit, recovered after a crash.
- Optional DebugInput logging, off by default.

## 0.1.0

- First release.
- Drop-in `xinput1_3.dll` proxy; all XInput calls forwarded to the system DLL.
- Fight mode: stick-flick and D-pad slashes as emulated touch swipes, dodges, block, auto-stab, magic with a slot layer, super, clash mash, final strike, pause and cutscene fast-forward.
- Menu mode: right-stick cursor, click and drag, back, confirm, stick and D-pad scrolling.
- R3 toggles modes with rumble feedback; optional automatic switching.
- All bindings and tuning in `ib3pad.ini`; diagnostics in `ib3pad.log`.
