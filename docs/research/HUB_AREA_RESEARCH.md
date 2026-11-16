# Saved hub destination (15.23)

The automatic create path bypasses the GUI handler at `140ACC140`. That handler reads selected save DWORD `+2691A4`, indexes the 12-byte table at `1430AC950`, and writes its first DWORD to `sMhArea+D344` before dispatching the lobby UI action. The saved lobby settings loader `140620C30` does not perform this step.

Table entries 0..5 map to `0, 12D, 12E, 12F, 131, 132` (hex). Entry zero lets the title use saved previous area `+269150`. The following table data is a sentinel, not another selectable destination. GUI formatting at `140ACBC70` and `140ACC5E0` separately displays the previous area for option zero.

`14025FDE0` consumes `D344`, applies native progression/area checks, falls back to `+269150` where appropriate, then calls `141AC0FC0`. It clears `D344` during cleanup. `141367AC0` updates `+269150` with normalized base destinations, explaining why it cannot replace the distinct start-location option.

Fix: in hub-only automatic startup, restore the omitted menu assignment immediately before native session creation, reading the actual game table. Reject out-of-range preference indices. Do not modify persisted data or native area validation. Screen-only, quest mode, and later manual title entry do not use this helper.

Evidence: semantic MCP get_function for the above functions; read-only Ghidra outputs in chat work/hub-preference.log and hub-preference-fields.log. Build and source guard passed; the user confirmed successful saved-area routing in-game. Individual progression/option combinations have not all been tested.
