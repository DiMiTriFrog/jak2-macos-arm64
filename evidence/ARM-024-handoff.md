# ARM-024 handoff — Jak II nativo macOS ARM64

## Estado real

La pantalla negra/Dolby quedó resuelta. La causa era la implementación ARM64 de
`pextlw/pextuw` usada por `matrix-transpose!`: se emitían `UZP1/UZP2` (desentrelazado)
cuando la semántica x86 del proyecto exige `ZIP1/ZIP2` (entrelazado). Eso corrompía
`camera-rot`, producía la geometría diagonal y dejaba la carga visual atascada.

El ejecutable ARM64 ya muestra título, menú, selección de guardado, campaña y
gameplay; la evidencia visual está en `logs/ARM-024/clean-title-window.png`,
`logs/ARM-024/clean-gameplay-window.png` y
`logs/ARM-024/clean-gameplay-input-active.png`.

Esos logs y capturas se conservan sólo en la máquina de desarrollo y están
excluidos del repositorio privado para no publicar rutas, dumps ni material
derivado. La tabla inferior conserva el resultado verificable resumido.

No se declara todavía el 100 % de campaña, guardado, periféricos, firma,
notarización o Gatekeeper. Esas son puertas posteriores del proyecto.

## Contexto reproducible

- Proyecto: raíz del repositorio privado `jak-project`
- Rama: `codex/arm-024-full-jak2`
- Commit base: `d154e5f53`
- Host: Apple M3 Pro, `arm64`, macOS `26.5.2`
- Rosetta: no interviene; `sysctl.proc_translated = 0`
- ISO/assets: suministrados por el propietario y mantenidos sólo localmente; no se incluyen en Git.

Arranque correcto:

```text
build-arm64-release/game/gk.app/Contents/MacOS/gk -g jak2 --proj-path . -- -boot -fakeiso
```

## Cambios determinantes

1. `goalc/emitter/IGenARM64.cpp`: los seis emisores `pext*` ahora usan las
   codificaciones AArch64 `ZIP1/ZIP2` correspondientes a byte/halfword/word.
2. `test/test_emitter_arm64_parity.cpp` y
   `test/goalc/test_arm64_ir_int128.cpp`: las expectativas ahora verifican la
   semántica correcta de entrelazado.
3. `test/goalc/framework/test_runner.cpp`: el listener Jak 2 usa el puerto
   por versión (`8113`), igual que el runtime real; el valor `8112` causaba el
   falso bloqueo de las pruebas del kernel.
4. `goal_src/jak2/kernel/gkernel.gc`: el suelo ARM64 de las pilas de respaldo es
   `768`, dejando espacio para el objeto `cpu-thread` en el heap de prueba mínimo
   de 1024 bytes. x86 no cambia.

## Pruebas verificadas

| Prueba | Resultado | Evidencia |
|---|---:|---|
| Paridad completa del emitter ARM64 | 27/27 | `logs/ARM-024/clean-emitter-parity-all.log` |
| Regresiones `pext` IR + emitter | 28/28 | `logs/ARM-024/clean-pext-regressions.log` |
| Suite amplia sin `Jak2KernelTest.*` | 871/871 | `logs/ARM-024/clean-goalc-test-excluding-jak2-kernel.log` |
| `Jak2KernelTest.Basic` | 1/1 | `logs/ARM-024/clean-jak2-kernel-basic.log` |
| Smoke nativo con display | llega a `GAMEPLAY: enter ctysluma`; timeout controlado | `logs/ARM-024/final-native-smoke.log` |
| Build x86-64 | código 0 | `logs/ARM-024/final-x86-build.log` |

El timeout `124` del smoke es intencionado: mantiene el juego vivo durante la
prueba y no es un crash.

## Auditoría ARM64/Game Mode

- `gk.app/Contents/MacOS/gk`: Mach-O 64-bit `arm64`.
- `lipo -archs`: sólo `arm64`.
- Mach header: `ARM64`, `PIE`.
- `Info.plist`: ejecutable `gk`, bundle id `org.openggoal.jak2`,
  `LSSupportsGameMode=true`.
- Las dependencias dylib del paquete deben auditarse también si se prepara una
  distribución universal; no se firma ni notariza en ARM-024.

## Bloqueo separado, no visual

`Jak2KernelTest.RunFunctionInProcess` todavía termina con `SIGSEGV` en el fixture
artificial que crea dos procesos de 1024 bytes y les asigna la misma
`*kernel-dram-stack*`. El fallo aparece después de `child-proc activated`, en la
limpieza ARM64 de `catch-frame`; `Jak2KernelTest.Basic` sí pasa. No se debe usar
este fallo de fixture para reabrir el diagnóstico de Dolby/renderizador.

Reproducción:

```text
gtimeout 180 ./build-arm64-release/goalc-test --gtest_color=no \
  '--gtest_filter=Jak2KernelTest.RunFunctionInProcess'
```

Evidencia: `logs/ARM-024/clean-jak2-run-function-after-real-stack-save.log` y
`logs/ARM-024/jak2-catch-frame-stack-save-lldb.log`.

Antes de tocar producción, Luna debe aislar si el fixture necesita pilas únicas
por proceso o si el contexto de `throw 'initialize #f` conserva mal `sp/ra`.
No debe añadir `sleep`, offsets observados, desactivar aserciones, ni introducir
otra modificación al renderer.

## Higiene

- `git diff --check`: correcto.
- No se ejecutó ningún binario x86; sólo se compiló para verificar no regresión.
- No se usó Rosetta, SIP desactivado, W^X desactivado ni protecciones de macOS relajadas.
- No se hizo `push`, firma, notarización ni publicación.
