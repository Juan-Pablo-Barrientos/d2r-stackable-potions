# Stackable Potions

Potions stack in one inventory cell for Diablo II: Resurrected, the way keys
do. A potion you pick up joins a stack you already carry, and drinking from a
stack uses one potion.

It has two parts, and you need both:

| Part | What it does |
| --- | --- |
| `StackablePotions\` | A [D2RMM](https://github.com/olegbl/d2rmm) mod. It changes the game data so potions can stack. |
| `d2rl-stackable-potions.dll` | A [D2RLoader](https://d2rloader.net) plugin. Without it, drinking from a stack uses the whole stack. It also merges loose potions into stacks. |

## Install

Download `StackablePotions.zip` from the [latest release](../../releases/latest).

1. Install [D2RLoader](https://d2rloader.net). Tested with 1.3.1-beta.
2. Copy `d2rl-stackable-potions.dll` into `<game folder>\d2rloader\plugins\`.
3. Copy the `StackablePotions` folder into D2RMM's `mods` folder. Enable it,
   choose your settings, and click **Install Mods**.
4. Start the game with D2RLoader using D2RMM's output mod. For example, set
   `default_mod = "D2RMM"` in `d2rloader\config\d2rloader.toml`.

D2RMM can't install the DLL: it only writes inside its own output mod, and
D2RLoader loads plugins from `d2rloader\plugins`. Copy the DLL again only when
the plugin updates.

To remove it, disable the mod in D2RMM, click **Install Mods**, and delete the
DLL. Stacks you already have stay stacked.

## D2RMM settings

| Setting | What it does |
| --- | --- |
| Stack size | The most potions one stack holds, 2 to 511. Default 50. |
| Stack on pickup | A potion you pick up joins a matching stack in your inventory. |
| Rejuvenation potions | Rejuvenation and Full Rejuvenation potions stack too. |
| Stamina, Antidote and Thawing potions | These potions stack too. |

The mod changes `misc.txt` (`stackable`, `minstack`, `maxstack`, `spawnstack`,
and `compactsave 0` so the stack size is saved) and sets `AutoStack` in
`itemtypes.txt`, the setting that makes keys merge when you pick them up. Shops
and monsters still give one potion at a time.

## What the plugin does

- **Drinking from a stack uses one potion.** D2R drinks a potion by deleting
  the whole item, so on its own it would drink a stack of 20 as one potion.
  The plugin puts a stack one smaller back right away, in the same inventory
  cell or the same belt column.
- **Loose potions merge.** D2R doesn't stack potions bought from a vendor, or
  potions you had before installing the mod. About 1.5 seconds after your
  inventory stops changing, the plugin merges potions of the same kind into
  as few stacks as possible.

The plugin reads each potion's stack size from the game data. Without the
D2RMM mod it does nothing.

It only counts a drink when it is sure one happened, so moving, selling or
dropping a stack never gives potions back:

- **Inventory:** the stack has to vanish within two seconds of a plain
  right-click, with no left-click since. Ctrl, Shift and Alt clicks don't
  count.
- **Belt:** the stack has to vanish without turning up anywhere else.
  Pressing several belt keys at once works: each stack is put back in its
  own column, one after another. If two items vanish from the same column,
  something other than a belt potion vanishes with them, or the belt itself
  is removed or swapped (death too), nothing is put back.

It works in single player and when you host a TCP/IP game. It does nothing
when you join someone else's game, because only the host can change items.

## Plugin settings

The first launch creates `<game folder>\d2rloader\config\stackable-potions.toml`:

```toml
[stackable-potions]
# Drinking from a stack uses one potion. The rest of the stack stays.
enabled = true

# Merge loose potions in the inventory into stacks.
merge_loose = true

# Write every restored stack and merge to d2rloader\logs\stackable-potions.log.
log_actions = false
```

The console command `stackable-potions-dump` lists every potion with its stack
size.

## Your characters

Character saves from D2R 3.0 and later (format v105) store a quantity on each
item only when it has one. Turning stacking on or off can't misread items
already in a save. Potions saved before the mod have no quantity, which is why
the plugin merges them into new stacks. D2RLoader also backs up offline
characters before their first save in each session.

## Compatibility

- Works alongside [Potion Refill](https://github.com/Juan-Pablo-Barrientos/d2r-potion-refill):
  when a belt column runs low, Potion Refill moves a whole stack into it.
- A belt stack is put back by creating it on the cursor and asking the game to
  place it in the belt. That uses a D2RLoader function that is not part of the
  public plugin SDK. The plugin checks it on startup. If a D2RLoader update
  changes it, drunk belt stacks go to the inventory instead, and the log says
  so.
- The stack size is read from the game's compiled item table. The plugin
  checks the table layout against the key row on startup and stays off if it
  changed.

## Build from source

You need Visual Studio 2022 with the C++ desktop tools and a Windows 10/11 SDK.

```powershell
.\build.ps1                 # builds build\d2rl-stackable-potions.dll
.\build.ps1 -Install -GameDir "D:\Diablo II Resurrected"
.\package.ps1               # builds dist\StackablePotions.zip
```

The D2RLoader plugin SDK headers are included in `third_party/PluginSDK`
(MIT license). `src/compat/span` stands in for `<span>` on toolchains that
lack it.

## License

MIT. See [LICENSE](LICENSE).
