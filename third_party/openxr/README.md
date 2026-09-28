# OpenXR headers

The core OpenXR 1.1 API headers, used by the Meta Quest build (`frontend/vr/vr_openxr.cpp`).

Taken unmodified from the Khronos `org.khronos.openxr:openxr_loader_for_android:1.1.63`
package on Maven Central, the same package the `quest` Gradle flavor ships the loader library
from. Licensed `Apache-2.0 OR MIT` (see `LICENSE`).

Only headers live here. The loader is opened at runtime with `dlopen`, so non-Quest builds never
depend on it.
