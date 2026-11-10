#pragma once

class UnitClass;

namespace venture::features::example
{
// Called from the UnitClass ctor/dtor hooks (see ExampleFeature.cpp). These are
// the module's own hook callbacks; the hook bodies stay in the .cpp so the
// header stays free of engine headers.
void OnUnitCreated(UnitClass* pUnit);
void OnUnitDestroyed(UnitClass* pUnit);
}
