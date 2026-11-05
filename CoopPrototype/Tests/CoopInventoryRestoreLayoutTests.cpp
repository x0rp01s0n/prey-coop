#include "CoopInventoryRestoreLayout.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using CoopInventoryRestoreLayout::ClassifyPendingEntity;
using CoopInventoryRestoreLayout::PendingAction;
using CoopInventoryRestoreLayout::TryPlaceAtSavedCell;

struct Cell
{
    unsigned itemId;
    int x;
    int y;
};

class FakeInventory
{
public:
    std::vector<Cell> cells;
    bool allowPlace = true;
    int placeCalls = 0;

    bool ReadCells(std::vector<Cell>& out, std::string&)
    {
        out = cells;
        return true;
    }

    bool TryPlace(unsigned itemId, int x, int y, bool& placed, std::string&)
    {
        ++placeCalls;
        placed = allowPlace;
        if (!placed)
            return true;
        for (Cell& cell : cells)
        {
            if (cell.itemId == itemId)
            {
                cell.x = x;
                cell.y = y;
                return true;
            }
        }
        placed = false;
        return true;
    }
};

bool Place(FakeInventory& inventory, unsigned id, int x, int y, std::string& reason)
{
    return TryPlaceAtSavedCell<Cell>(
        id,
        x,
        y,
        [&inventory](std::vector<Cell>& cells, std::string& readReason)
        {
            return inventory.ReadCells(cells, readReason);
        },
        [&inventory](unsigned itemId, int targetX, int targetY, bool& placed, std::string& placeReason)
        {
            return inventory.TryPlace(itemId, targetX, targetY, placed, placeReason);
        },
        reason);
}

void TestPlacementPreservesAndVerifiesSavedCell()
{
    FakeInventory inventory{{{7, 1, 1}}};
    std::string reason;
    assert(Place(inventory, 7, 1, 1, reason));
    assert(inventory.placeCalls == 0);

    assert(Place(inventory, 7, 4, 2, reason));
    assert(inventory.placeCalls == 1);
    assert(inventory.cells[0].x == 4 && inventory.cells[0].y == 2);
}

void TestFailedPlacementIsNotPackedElsewhere()
{
    FakeInventory inventory{{{9, 1, 1}}};
    inventory.allowPlace = false;
    std::string reason;
    assert(!Place(inventory, 9, 3, 2, reason));
    assert(inventory.placeCalls == 1);
    assert(inventory.cells[0].x == 1 && inventory.cells[0].y == 1);
    assert(!reason.empty());
}

void TestMissingCoordinatesAndMissingItem()
{
    FakeInventory inventory{{{11, 2, 3}}};
    std::string reason;
    assert(Place(inventory, 11, -1, -1, reason));
    assert(inventory.placeCalls == 0);
    assert(!Place(inventory, 12, 0, 0, reason));
}

void TestPendingRestoreClassification()
{
    assert(ClassifyPendingEntity(true, true, true, true) == PendingAction::PlacementOnly);
    assert(ClassifyPendingEntity(false, false, false, false) == PendingAction::Respawn);
    assert(ClassifyPendingEntity(false, true, false, false) == PendingAction::FailClosed);
    assert(ClassifyPendingEntity(true, false, true, true) == PendingAction::FailClosed);
    assert(ClassifyPendingEntity(true, true, false, false) == PendingAction::FailClosed);
    assert(ClassifyPendingEntity(true, true, true, false) == PendingAction::FailClosed);
}
}

int main()
{
    TestPlacementPreservesAndVerifiesSavedCell();
    TestFailedPlacementIsNotPackedElsewhere();
    TestMissingCoordinatesAndMissingItem();
    TestPendingRestoreClassification();
    std::cout << "inventory restore layout tests passed\n";
    return 0;
}
