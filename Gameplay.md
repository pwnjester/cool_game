**Abilites**
-Rather than "learning" permanant abilties that have cooldowns, you would collect cards through defeating enemies and crafting them through Elixir. Each card collected can ONLY be used once. If you find the same card again, the cards would "stack" and would give you more than one use. (ex. If I had a card, and I found the same card, they would stack and give me 2 uses for the card.) Cards can ONLY stack if they are the SAME. Depending on how good the card is, the card would have a stack limit. (Ex: If it is a weak card, it could stack up to 10 or 15 times, but if it is a strong card, it could only stack up to 3-5 times, maybe even less.) If you do not have space to stack a card, you can either leave it, or you can convert it into Elixr for later crafting use.

## Controls

- Movement: Arrow keys or `WASD`
- Attack / Interact: `Space` (melee attack; deals damage to nearest NPC in range)
- Interact / Talk: `E` (when near an NPC with dialog)
- Open Inventory (future): `I`
- Restart after Game Over: `Enter`

## Level token syntax (examples)

- `00` — floor tile (default)
- `01`, `02`, ... — numeric tile tokens map to `assets/tiles/01.png`, etc.
- `A`, `B`, ... — entity tokens (NPCs) map to `assets/entities/A.png`
- `P` — player spawn
- Parentheses allow extra data: `A(00)` places NPC A over tile `00`.
- NPC options: comma-separated inside parentheses: `A(hostile,hp=20,drop=C01,lvl=2,say="Hello")`
	- `hostile` — makes the NPC hostile
	- `hp=#` — set HP
	- `drop=ID` — card/item ID to drop on death
	- `lvl=#` — grant this many levels to player on kill
	- `say="..."` — dialog string shown when interacting

Example: `A(hostile,hp=12,drop=C02,lvl=1,say="You'll regret this!")`
doesnt work rn though
