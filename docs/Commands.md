# Commands

The platform's player commands: a small registry of command descriptors
(`src/Command/Commands.h`) and one engine `CommandClass` object per enabled
descriptor, created when the engine asks the injected DLLs to register their
commands. A registered command appears in the game's keyboard configuration
under its UI category, and the player binds a key to it - the same convention
every Phobos-lineage DLL uses.

Related documents: `docs/HookCatalogue.md` (the engine hook this layer
installs), `docs/EventChannel.md` (the event the example command raises),
`docs/EngineBindings.md` (the binding layers).

## How registration works

1. The platform registers command descriptors at load time (a static
   initializer / `Registrar` object in the owning module is the normal place -
   before the engine asks for its command list):

   ```cpp
   #include "Command/Commands.h"

   void ExecuteMyCommand()
   {
       // Runs on the game thread when the player presses the bound key.
   }

   const venture::Commands::RegisterResult registered = venture::Commands::Register({
       "My Command",                 // engine name: unique, the keyboard config keys on it
       L"My Command",                // name shown in the keyboard dialog
       L"MyCategory",                // dialog category (wide string, like the engine wants)
       L"What the command does",     // dialog description
       true,                         // enabled: false keeps it out of the engine list
       &ExecuteMyCommand,            // handler
   });
   ```

   `Register` returns `Ok`, `InvalidCommand` (a required field is null/empty),
   `PlatformRefused` (the platform refused to start, see `docs/Lineage.md` - the
   registry is closed) or `DuplicateName`.  Names are unique and compared
   **case-insensitively**: the engine and every injected DLL share one flat
   command namespace, so `My Command` and `my command` are the same name and the
   second registration is rejected.  The descriptor struct
   is copied into the registry, but its **pointers are borrowed, not deep-copied**:
   `name` / `uiName` / `uiCategory` / `uiDescription` must point at
   static-lifetime storage (string literals are the intended use) and `handler`
   must be a function that stays valid for the process lifetime - the engine keeps
   its own command object and may call `Execute` / `GetUIName` at any later time.
   Never pass a temporary buffer such as `std::string::c_str()`.
   `Commands::At(index)` / `Commands::Count()` iterate it in registration order,
   `Find(name)` / `SetEnabled(name, on)` / `IsEnabled(name)` look one up by
   engine name, case-insensitively.

2. The engine calls its command-registration callback once while it builds its
   own command list. The platform hooks that point (`0x533066`, size 6 -
   `CommandClassCallback_Register`; windows and evidence in
   `docs/HookCatalogue.md`) and, for every **enabled** entry:

   * creates one `PlatformCommandClass` object with the engine allocator
     (`GameCreate`, like the engine's own command objects);
   * forwards `GetName` / `GetUIName` / `GetUICategory` / `GetUIDescription`
     to the descriptor, `Execute` to the descriptor's handler, and
     `ExtraTriggerCondition` to "key-down only";
   * appends it with `CommandClass::Array.AddItem(...)` - the same mechanism the
     reference `MakeCommand<T>()` helper uses in the Phobos-lineage ecosystem;
   * logs the command name, category and UI name.

   A disabled entry is logged as skipped and never reaches the engine.  The hook
   finishes with a summary line (`N command(s) appended to the engine command
   list`).

3. The engine's own registration routine keeps appending its command objects
   **after** that hook point, so a second, post-registration hook runs once the
   list is final: `0x533C3E`, size 5 - the `call 0x533D20` inside the same
   routine that rebuilds `CommandClass::Hotkeys` (window and free-site evidence
   in `docs/HookCatalogue.md`).  It snapshots the completed list, logs one
   summary line (`commands: post-registration check - ...`) and warns about
   every name collision that involves a platform command, in both directions
   (see the next section).  Its handler returns 0, so the trampoline re-executes
   the call and the engine's hotkey rebuild is unchanged.

### Name collisions and the platform prefix

The engine's command list is one flat namespace shared by the engine and every
injected DLL, and lookups are first-match.  The platform's registration hook
appends at `0x533066`, which is **mid-way** through the engine's own inline
registration sequence, so a command the engine (or another DLL) registers after
that point is not visible from there.  The post-registration hook `0x533C3E`
(see above) runs once the routine has appended its last command and before the
hotkey map is rebuilt; it snapshots the final list and reports every collision
that involves a platform command - in both directions:

* a platform command that comes **first** in the list shadows the other command
  (the game's lookup stops at the first match) - the warning names both entries
  and says the other command is shadowed;
* a platform command that comes **later** is the one shadowed - the warning says
  so and asks for a rename.

Commands of other injected DLLs are part of the same namespace, so they are
covered too.  The scan is diagnostic only: by the time it runs the engine owns
the list, it never removes or renames entries.  That is why the convention is
mandatory:

> **Name every platform command with the platform prefix**, for example
> `"Venture Test Command"` (engine name) for `VentureCore`.  A derived platform
> renames the prefix together with everything else (see `docs/Renaming.md`).
> Never register a generic name such as `"Select All"` or a command that a
> vanilla host may also define.

The registry also rejects a second platform command whose name differs only in
case, which keeps the two sides of the configuration unambiguous.

Every hook handler returns 0, so Syringe's trampoline re-executes the
instruction the hook overwrote: the command-object store
(`mov DWORD PTR [eax],0x7eb89c`) at the shared `0x533066` site, and the
`call 0x533D20` at the post-registration site.  The platforms chained at the
registration site (Phobos, Kratos, Mix and other ecosystem platforms) keep
working.

## Hotkeys

The engine's command objects do not carry a default key.  At the end of its own
command-registration routine the engine calls `0x533D20`, which rebuilds
`CommandClass::Hotkeys` from the `[Hotkey]` section of `KEYBOARDMD.INI` (the map
is cleared, then filled from the keyboard configuration); the platform's
post-registration check hooks that call inside the routine (`0x533C3E`, size 5)
and therefore observes the final command list immediately before the rebuild.
A binding added during the registration callback would be thrown away, so the
platform does **not** hard-code one: like every other command in the ecosystem,
the command shows up in **Options -> Keyboard**, and the player assigns the key
there.  The binding is stored in the game's keyboard configuration and survives
restarts.

**Suggested binding for the example command: `Ctrl+Shift+V`** (or any free key).

To bind it:

1. Start a game (single player is enough for the test below).
2. **Options -> Keyboard**, find the category **VentureCore**.
3. Select **Venture Test Command**, press `Ctrl+Shift+V`, confirm, close.
4. Back in the game, press `Ctrl+Shift+V`.

## The example command

`src/Engine/Hooks.Commands.cpp` registers one demo command and one demo event at
load time:

| Item | Value |
| --- | --- |
| command name | `Venture Test Command` |
| UI category / name | `VentureCore` / `Venture Test Command` |
| suggested hotkey | `Ctrl+Shift+V` (bound by the player, see above) |
| event | `0x70` `Venture_Event_Test`, 8-byte payload `{ magic 'VENT', sequence }` |

When the command runs it:

1. logs `[Venture] command "Venture Test Command" executed`;
2. raises event `0x70` through the event channel
   (`EventChannel::Send`, current player house, current engine frame);
3. the engine queues the event, the receive path dispatches it back - including
   on the local machine - and the event responder logs
   `[Venture] event 0x70 received (sequence=N) - command and event paths are live`.

The event is only raised when there is a current player: `HouseIndex = -1` is
never sent (HOOK-08).  The engine's event dispatch entry sign-extends the house
index (`0x4C6CC4 movsx ebx,byte [esi+2]`) and indexes `HouseClass::Array` with it
unconditionally at `0x4C6CCA`, before the event-type switch, so a negative index
would be a 4-byte out-of-bounds read on every receiver.  If `CurrentPlayer` is
null (a headless/early call) the command logs one warning and drops the event
instead of creating that index.

So one key press proves the whole path: command hook -> descriptor ->
`Execute` -> event channel -> frame codec -> engine event queue -> dispatch
hook -> responder. The event side is described in `docs/EventChannel.md`.

Expected log excerpt (startup and after the key press; `N` is the engine's
command count and `M` the platform's enabled command count):

```
commands: M command(s) in the platform registry
commands:   "Venture Test Command" (category "VentureCore", ui name "Venture Test Command") registered
commands: M command(s) appended to the engine command list
commands: post-registration check - M platform command(s) among N engine-list entry(s), 0 name conflict(s)
event channel: 0x70-0x7F, 1 registered handler(s)
event channel:   0x70 Venture_Event_Test        8 byte(s)
...
[Venture] command "Venture Test Command" executed
[Venture] event 0x70 received (sequence=1) - command and event paths are live
```

## What to test in the game

1. Startup log contains the `commands:` lines above (including the
   `post-registration check` summary) and no warning about the example command
   or any name collision.
2. Options -> Keyboard shows **Venture Test Command** under **VentureCore**;
   the description is visible.
3. After binding `Ctrl+Shift+V`, pressing it in a single-player game writes the
   two `[Venture]` lines (command and event) to `<Platform>.log`.
4. Vanilla commands keep working and other DLLs' commands still appear; the
   platform must not disturb them (all handlers return 0 at the shared site),
   and the hotkey rebuild still runs after the post-registration check.
5. Co-load (Ares/Phobos/Kratos/Mix): the post-registration summary is still
   logged once, no collision warning names a real ecosystem command, and every
   DLL's commands are visible in Options -> Keyboard.
6. Multiplayer: commands are local triggers; anything that has to happen on
   every machine must raise a platform event (see the mixed-client warning in
   `docs/EventChannel.md`).

## Limits

* No default hotkey - see the hotkey section (the engine reloads the key map
  after the registration callback).
* The registry is fixed at load time: commands cannot be added or removed
  after the engine built its list (the enabled flag is read once, by the
  registration hook).
* Name-collision detection is diagnostic only and runs after registration: it
  warns about both shadowing directions (including commands of other injected
  DLLs) but cannot rename or remove anything.
* The example command raises no event when there is no current player - a
  missing event is logged as one warning instead of sending `HouseIndex = -1`
  (HOOK-08, see above).
* `enabled` is a platform switch only; it is not exposed to the player as a
  runtime toggle.
* UI strings are wide literals in the DLL; there is no CSF/localisation
  integration yet.
* Commands are not network-synchronised by themselves: a command either has
  local-only effect or raises an event for the effect that has to run on every
  machine.
* There is no dedicated unregister path: the engine command array keeps the
  objects for the process lifetime (they are never freed, like vanilla
  commands).
