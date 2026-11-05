#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace CoopInventoryRestoreLayout
{
enum class PendingAction
{
    Respawn,
    PlacementOnly,
    FailClosed,
};

inline PendingAction ClassifyPendingEntity(
    bool entityExists,
    bool inInventory,
    bool archetypeKnown,
    bool archetypeMatches)
{
    if (!entityExists)
        return inInventory ? PendingAction::FailClosed : PendingAction::Respawn;
    if (!archetypeKnown || !archetypeMatches || !inInventory)
        return PendingAction::FailClosed;
    return PendingAction::PlacementOnly;
}

template <typename Cell, typename ReadCells, typename TryPlace>
bool TryPlaceAtSavedCell(
    unsigned itemId,
    int x,
    int y,
    ReadCells readCells,
    TryPlace tryPlace,
    std::string& reason)
{
    auto findCell = [itemId](const std::vector<Cell>& cells)
        -> const Cell*
    {
        const auto it = std::find_if(
            cells.begin(),
            cells.end(),
            [itemId](const Cell& cell)
            {
                return cell.itemId == itemId;
            });
        return it == cells.end() ? nullptr : &*it;
    };

    std::vector<Cell> cells;
    if (!readCells(cells, reason))
        return false;

    const Cell* currentCell = findCell(cells);
    if (!currentCell)
    {
        reason = "restored item is not in stored cells: " + std::to_string(itemId);
        return false;
    }
    if (x < 0 || y < 0 || (currentCell->x == x && currentCell->y == y))
        return true;

    bool placed = false;
    if (!tryPlace(itemId, x, y, placed, reason))
        return false;

    if (!readCells(cells, reason))
        return false;

    currentCell = findCell(cells);
    if (currentCell && currentCell->x == x && currentCell->y == y)
        return true;

    reason = "TryPlaceItem did not restore saved cell item=" + std::to_string(itemId) +
        " target=" + std::to_string(x) + "," + std::to_string(y) +
        (currentCell ? " current=" + std::to_string(currentCell->x) + "," + std::to_string(currentCell->y) : " current=missing") +
        " returned=" + (placed ? "1" : "0");
    return false;
}
}
