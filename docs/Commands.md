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

1. The platform registers command descriptors at load time:

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

   `Register` returns `Ok`, `InvalidCommand` (a required field is null/empty) or
   `DuplicateName`. Names are unique and compared exactly. The descriptor is
   copied into the registry; `Commands::At(index)` / `Commands::Count()` iterate
   it in registration order, `Find(name)` / `SetEnabled(name, on)` /
   `IsEnabled(name)` look one up by engine name.

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

   A disabled entry is logged as skipped and never reaches the engine.

Every hook handler returns 0, so Syringe's trampoline re-executes the
instruction the hook overwrote (`mov DWORD PTR [eax],0x7eb89c`), and the
platforms chained at the same site (Phobos, Kratos, Kaleido, Mix) keep working.

## Hotkeys

The engine's command objects do not carry a default key. Right after the
registration callback, the engine rebuilds `CommandClass::Hotkeys` from the
`[Hotkey]` section of `KEYBOARDMD.INI` (verified in the engine at `0x533D20`:
the map is cleared, then filled from the keyboard configuration). A binding
added during the registration callback would therefore be thrown away, so the
platform does **not** hard-code one: like every other command in the ecosystem,
the command shows up in **Options -> Keyboard**, and the player assigns the key
there. The binding is stored in the game's keyboard configuration and survives
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

So one key press proves the whole path: command hook -> descriptor ->
`Execute` -> event channel -> frame codec -> engine event queue -> dispatch
hook -> responder. The event side is described in `docs/EventChannel.md`.

Expected log excerpt (startup and after the key press):

```
commands: 1 command(s) in the platform registry
commands:   "Venture Test Command" (category "VentureCore", ui name "Venture Test Command") registered
event channel: 0x70-0x7F, 1 registered handler(s)
event channel:   0x70 Venture_Event_Test        8 byte(s)
...
[Venture] command "Venture Test Command" executed
[Venture] event 0x70 received (sequence=1) - command and event paths are live
```

## What to test in the game

1. Startup log contains the `commands:` lines above and no warning about the
   example command.
2. Options -> Keyboard shows **Venture Test Command** under **VentureCore**;
   the description is visible.
3. After binding `Ctrl+Shift+V`, pressing it in a single-player game writes the
   two `[Venture]` lines (command and event) to `<Platform>.log`.
4. Vanilla commands keep working and other DLLs' commands still appear; the
   platform must not disturb them (all handlers return 0 at the shared site).
5. Multiplayer: commands are local triggers; anything that has to happen on
   every machine must raise a platform event (see the mixed-client warning in
   `docs/EventChannel.md`).

## Limits

* No default hotkey - see the hotkey section (the engine reloads the key map
  after the registration callback).
* The registry is fixed at load time: commands cannot be added or removed
  after the engine built its list (the enabled flag is read once, by the
  registration hook).
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
