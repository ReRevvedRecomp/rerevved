# AI handicaps

The Gameplay settings include AI handicaps for the next new game. There are
five AI bonus percentages: **AI Research Bonus**, **AI Production Bonus**,
**AI Gold Bonus**, **AI Food Bonus** and **AI Culture Bonus**. Each runs from 0
to 200 and defaults to 0, which keeps the game's native behavior.
Human players and Barbarians always keep their native values.

1. Set the bonus percentages you want between 0 and 200.
2. Start a new game. Changing the settings during a game changes the next new
   game. Multiplayer uses the host's values. See
   [Online play](online-play.md#gameplay-settings-and-mods).

## Research and production

These bonuses make computer opponents research and produce items sooner. A
bonus of 100 halves the native cost, and 200 divides it by three. The result is
the native cost times 100, divided by 100 plus the bonus, truncated. A positive
cost stays at least one.

Research scales the final native technology cost after its existing modifiers
and rounding.

Unit production scales the native cost scalar after existing discounts and
mod rules. The game then multiplies that scalar by the unit's production
factor and divides by two. Integer rounding means a bonus can produce a
different reduction in the displayed cost. An already positive cost stays
positive. Building and Wonder production scales the final native cost, with
a minimum of one for positive costs. Existing native discounts still apply.
The supported non-unit items are ordinary buildings 100 through 122 and
Wonders 200 through 232. Other item IDs retain their native costs.

## Gold, food and culture

These bonuses raise what each AI city contributes. The game multiplies the
completed native contribution by 100 plus the bonus, divides by 100 and
truncates. The value is changed at the point where the game stores it, so the
city totals, the nation's credits and the later native clearing of excluded
owners follow the changed value.

- Only positive contributions are boosted. Zero and negative values stay native.
- **AI Food Bonus** boosts the completed growth contribution of a city, not the
  gross harvest. Starvation is never reduced or increased.
- A scaled value is capped at 32767, the largest value the city record holds.
  A native value already above 32767 is kept as it is.
- Gold and culture values depend on how the game composes them for each city.
  The percentage applies to the composed value, not to each source.

## Saves and multiplayer

The gameplay sidecar saves all five bonuses with the native save.
Loading a single-player save restores the saved values. Saves without a valid
sidecar, sidecars from earlier releases, and loads with sidecars disabled use 0
for all five.
Keep the `.rerevved-gameplay` file beside its native save when copying saves.

Game of the Week with **Standard Rules** uses 0 for all five bonuses.
Multiplayer loads use native behavior, whatever the save recorded. These
options change the economy. They do not select a leader personality or change
the number of AI civilizations.
