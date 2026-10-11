# Informe final de investigación (A–H)

Fecha: 2026-10-11. Resume y enlaza los documentos de `docs/research`; no los duplica. Etiquetas: **VERIFICADO**, **INFERIDO**, **PENDIENTE**.

## A. Identidad y migración ORVYN
El proyecto se llama ORVYN; CIDER es el nombre histórico. La migración está en `1b77aa7` y documentada en [CIDER-TO-ORVYN-MIGRATION.md](CIDER-TO-ORVYN-MIGRATION.md). La versión de CMake sigue en **0.5.0**. Quedan rastros conocidos: 340 snapshots dorados con rutas absolutas del repositorio antiguo (inofensivo para los tests).

## B. Estado Git
* `origin/main` = `02de140`; local `main` = `503390a` (un commit por delante, **sin publicar**). Hay un único remoto `origin`.
* Autoría: los tres commits del repo usan `RDMMDLC <58237652+dfnetwork@users.noreply.github.com>`, sin coautores de IA.
* El contenedor de esta sesión tiene configurada globalmente la identidad `Claude <noreply@anthropic.com>`; `503390a` se creó con la identidad de los commits anteriores pasada por `-c`. **Pendiente: que el propietario confirme que esa identidad es suya.**
* Hook `~/.claude/stop-hook-git-check.sh` (infraestructura de la sesión en la nube, no del repo): marca como "Unverified" los commits cuyo committer no es `noreply@anthropic.com` y pide reescribirlos con la identidad de Claude. No se ha aplicado ni desactivado.
* No se ha hecho `pull`, `reset`, `rebase`, `push` ni `push --force`.

## C. Investigación de Apple
Inventario, versiones, licencias y rutas: [APPLE-OPEN-SOURCE-INVENTORY.md](APPLE-OPEN-SOURCE-INVENTORY.md). Análisis por área: [dyld](DYLD-COMPATIBILITY.md), [libSystem/libc](LIBSYSTEM-LIBC-ANALYSIS.md), [libdispatch](LIBDISPATCH-RESEARCH.md), [Objective-C](OBJC-RUNTIME-COMPATIBILITY.md), [Foundation/CF](FOUNDATION-COREFOUNDATION.md), [AppKit/CG](APPKIT-COREGRAPHICS.md), [XNU](DARWIN-XNU-INTERFACES.md). AppKit, Foundation y CoreGraphics de Apple no son abiertos.

## D. Reutilización y adaptación
[REUSE-ADAPTATION-MATRIX.md](REUSE-ADAPTATION-MATRIX.md) y [THIRD-PARTY-LICENSE-REVIEW.md](THIRD-PARTY-LICENSE-REVIEW.md). Por defecto se reimplementa; el código APSL no se copia hasta cerrar la revisión legal (no es asesoría legal). No se ha copiado código de terceros.

## E. Viabilidad de Swift
[SWIFT-RUNTIME-FEASIBILITY.md](SWIFT-RUNTIME-FEASIBILITY.md): viable pero con inversión considerable (261 imports en el `libswiftCore` 5.0, C++ runtime, weak binding, desenrollado, `dlopen`, ICU, overlays cerrados). Las cuatro apps Swift analizadas están lejos de ejecutarse. SwiftUI queda fuera de cualquier horizonte.

## F. Compatibilidad real
**Ninguna app de Apple se ha ejecutado.** Medidas en [REAL-APP-COMPATIBILITY.md](REAL-APP-COMPATIBILITY.md). Novedad: el análisis estático de cliclick 5.1 ([../compatibility/CLICLICK-5.1.md](../compatibility/CLICLICK-5.1.md)) muestra que **no necesita** listas de métodos relativas ni weak binding, y que **sí usa `@try/@catch`**, que ORVYN rechaza al cargar. Eso corrige la valoración anterior de cliclick como objetivo corto.

## G. Roadmap 0.6–1.0
[ORVYN-ROADMAP.md](ORVYN-ROADMAP.md) y [TOP-20-COMPATIBILITY-IMPROVEMENTS.md](TOP-20-COMPATIBILITY-IMPROVEMENTS.md), más el resumen en [EXECUTIVE-SUMMARY.md](EXECUTIVE-SUMMARY.md). Hay que revisar el hito 0.7 (cliclick) según la decisión de la sección H.

## H. Próximas acciones
1. Confirmar la identidad Git del propietario para commitear (sección B).
2. Disponer de una máquina Windows x64 con MSVC/ml64 para compilar y ejecutar: **desde el contenedor Linux no se puede construir ni probar el runtime** (el CMake solo habilita el runtime en Windows; los fuentes portables tampoco compilan aquí por falta de `std::format`).
3. Decidir sobre cliclick: soporte mínimo de excepciones, trampa en tiempo de llamada, u otro binario.
4. Línea base de tests en Windows (Debug/Release) antes de tocar código.
5. Después, por orden: `Cocoa` + stubs honestos, libc (`getopt`, `nanosleep`, stack protector), `objc_sync_*`, clases Foundation que faltan; listas relativas y weak binding siguen siendo útiles para otras apps pero no desbloquean cliclick.
