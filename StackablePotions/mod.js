// Makes potions stack in one inventory cell, the way keys do.
// Keys stack because of two settings, and this mod gives potions both:
//   misc.txt      stackable/minstack/maxstack/spawnstack, and compactsave 0 so
//                 the stack size is saved (compact items have no quantity).
//   itemtypes.txt AutoStack, so a picked-up potion joins a stack you carry.

const maxStack = Math.max(2, Math.min(511, Math.floor(Number(config.maxStack) || 50)));

const potionTypes = ['hpot', 'mpot'];
if (config.rejuvenation) {
  potionTypes.push('rpot');
}
if (config.otherPotions) {
  potionTypes.push('spot', 'apot', 'wpot');
}

// RotW keeps the base game's tables in excel\base for non-RotW characters.
const excelDirs = ['global\\excel\\', 'global\\excel\\base\\'];

let potions = 0;
excelDirs.forEach((dir) => {
  const misc = D2RMM.readTsv(dir + 'misc.txt');
  if (!misc) return;
  misc.rows.forEach((row) => {
    // Unused rows such as "Small Red Potion" can't be drunk; leave them alone.
    if (potionTypes.indexOf(row.type) === -1 || row.useable != 1) return;
    row.compactsave = 0;
    row.stackable = 1;
    row.minstack = 1;
    row.maxstack = maxStack;
    // Shops and drops hand out one potion at a time, as before.
    row.spawnstack = 1;
    potions++;
  });
  D2RMM.writeTsv(dir + 'misc.txt', misc);

  if (!config.autoStack) return;
  const itemTypes = D2RMM.readTsv(dir + 'itemtypes.txt');
  if (!itemTypes) return;
  itemTypes.rows.forEach((row) => {
    if (potionTypes.indexOf(row.Code) !== -1) {
      row.AutoStack = 1;
    }
  });
  D2RMM.writeTsv(dir + 'itemtypes.txt', itemTypes);
});

console.log('Stackable Potions: ' + potions + ' potion rows stack up to ' + maxStack + '.');
