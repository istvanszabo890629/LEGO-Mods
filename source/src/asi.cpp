#include "bootstrap.h"

// Ultimate ASI Loader calls this after LoadLibrary has returned, outside the
// Windows loader lock. UAL 10.0.2 defaults to DontLoadFromDllMain=1 without an
// INI. If another loader config exists, it must retain that setting.
extern "C" void __cdecl InitializeASI() { loader::EnsureInitialized(); }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) loader::Attach(instance);
  return TRUE;
}
