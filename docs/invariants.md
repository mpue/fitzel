# Fitzel — Invarianten

Regeln, die **zwischen** Dateien gelten und deshalb in keiner einzelnen stehen
können. Wer eine davon bricht, liest in dem Moment fast nie die Datei, die sie
erklärt — das ist der ganze Grund für diese Seite.

Nichts hier ist geplant oder gewünscht; jede Zeile ist am Code geprüft, mit
Fundstelle. Was eine Datei ihrem eigenen Leser bereits sagt, steht hier nicht
noch einmal — sondern wird verlinkt.

Alle sechs teilen eine Eigenschaft: **ein Verstoß ist unsichtbar.** Nichts stürzt
ab, nichts wird rot. Der Effekt bleibt aus, die Farbe sitzt auf dem falschen
Face, der Export lädt nichts mehr, der Schatten verschwindet — Wochen später und
weit weg von der Änderung, die es verursacht hat.

---

## 1. Texture-Units

**Units sind pro Material, nicht global.** `Material::setTexture(name, tex, unit)`
setzt die Sampler-Uniform *und* bindet die Textur, und `apply()` macht beides bei
jedem Draw neu. Zwei Materialien dürfen dieselbe Unit verschieden belegen — es
gibt nur zwei harte Grenzen:

1. Nicht mit dem kollidieren, was der **Renderer bei jedem Draw** bindet.
2. Nicht mit sich selbst kollidieren.

### Was der Renderer für jedes Material bindet

Diese Units sind für Materialien **gesperrt** (`Renderer.hpp`, Zeilen 80–96):

| Unit | Was | Konstante |
| ---- | --- | --------- |
| 2 | `uEnvProbe` (samplerCube, immer gebunden) | `kEnvProbeUnit` |
| 7 | `uShadowMap` (Cascade-Depth-Array) | `kShadowMapUnit` |
| 12–15 | `uShadowCube0..3` (Punktlicht-Schatten) | `kPointShadowUnit` |
| 16, 17 | `uIrradiance`, `uPrefilter` (IBL) | `kIrradianceUnit`, `kPrefilterUnit` |
| 24–26 | `uLightGridR/G/B` | `kLightGridUnit` |

Env-Probe und IBL sind *immer* gebunden, auch wenn niemand sie liest — ein
ungebundener Sampler liest Unit 0, und ein `samplerCube` und ein `sampler2D` auf
derselben Unit sind ein Typkonflikt.

### Was Materialien selbst belegen

| Unit | Was | Wer |
| ---- | --- | --- |
| 0 | `uTexture` (Basisfarbe) | Objekt, Straße, Brücke, Billboard |
| 1 | `uNormalMap` | Objekt, Straße |
| 3 | `uEmissionMap` | Objekt, Straße, Straßen-Decals |
| 4 | `uWetMap` | nur Straße |
| 3 + n | `uLayerTex[n]` — Terrain-Albedo-Layer | nur Terrain |
| 18 + n | `uLayerNorm[n]` — Terrain-Layer-Normalen | nur Terrain |
| 8–11 | `uPaintTex[0..3]` — Mesh-Paint-Slots | nur bemalte Meshes |

Für etwas Neues ist ab **27** frei.

### Der Konflikt, der schon drinsteht

`uLayerTex[n]` bindet auf `3 + n` (`main.cpp`, Terrain-Layer-Block), und
`kMaxTerrainLayers` ist 6. Der **fünfte texturierte Layer (n = 4) landet also auf
Unit 7** — dort, wo der Renderer das Cascade-Array hält. Ein `sampler2D` und ein
`sampler2DArray` auf einer Unit: der Terrain-Schatten oder der Layer geht kaputt,
je nachdem, was der Treiber tut. Der Kommentar in `Renderer.hpp` beschreibt die
Belegung bereits so, wie sie gemeint war („3-6, 8-11"), also mit einer Lücke bei
7 — nur bindet der Code sie nicht. Beißt erst ab fünf **texturierten** Layern,
deshalb ist es bisher niemandem passiert.

---

## 2. Uniforms lecken zwischen Draws

Alle Objekt-Materialien teilen sich **ein** Shader-Programm. Ein `Material` lädt
nur die Uniforms hoch, die es selbst gesetzt hat — alles andere behält den Wert,
den der **vorige Draw** hineingeschrieben hat.

Das Beispiel steht im Code (`main.cpp`, gpuMats-Schleife): `uWindowGrid` wird
**in beiden Zweigen** geschrieben — „a material that leaves it alone inherits the
last building's facade". Ein Material, das den Aus-Fall wegließe, bekäme die
Fensterreihen des zuletzt gezeichneten Hauses auf die eigene Wand. Nichts daran
sieht falsch aus, bis jemand die Zeichenreihenfolge ändert.

**Regel: Wer eine Uniform *lesen* lässt, muss sie auf jedem Material schreiben —
auch den Aus-Fall.** Nicht „nur wenn an", sondern beide Zweige.

Der Renderer nimmt einem einen kleinen, festen Satz ab. Diese haben vor jedem
`material->apply()` einen definierten Grundwert (`Renderer.cpp`, ~Zeile 437):

`uReflectivity`, `uAlpha`, `uGlass`, `uHasNormalMap`, `uAlphaCutout`,
`uRoadFade`, `uRainRings`, `uHasWetMap`, `uWetReflect`, `uEmission`,
`uEmissionStrength`, `uHasEmissionMap`, `uEmissionUVScale`, `uWindowGrid`.

**Alles andere ist Sache des Materials.** Eine neue Uniform gehört in diese Liste
*oder* in jedes Material, das das Programm benutzt — nicht dazwischen.

---

## 3. Undo: `push` oder `pushApplied`

`History::push` ruft das `redo()` des Kommandos auf. Das ist genau richtig, wenn
das Kommando *die Änderung ist* (`AddEntityCmd`, `DeleteEntitiesCmd`). Es ist
falsch für die andere Hälfte der Aufrufstellen: die, die erst am Objekt arbeiten
und danach einen Vorher/Nachher-Schnappschuss zum Rückgängigmachen abgeben.

Für die weist `redo()` den „Nachher"-Schnappschuss über eine Entity, die bereits
in diesem Zustand ist. Das kostet eine Tiefkopie jeder Komponente — und weil
Zuweisen einer `ComponentList` die alten Komponenten zerstört und neue klont,
**ist danach jeder Zeiger in diese Entity tot.**

```cpp
MeshComponent* mc = e.components.get<MeshComponent>();
// ... in-place ändern ...
history.push(std::make_unique<ModifyEntityCmd>(before, e), document);
mc->mesh.faces[i];   // <-- Use-after-free, sieht völlig harmlos aus
```

**Regel:** Änderung schon im Dokument → `pushApplied`. Kommando *ist* die
Änderung → `push`. (`Command.hpp`)

Zwei Folgeregeln, die daraus fallen:

- **Panels fassen das Dokument nicht an.** Ein Panel meldet, was es will; der
  Host wendet es an, *nachdem* `drawPanel` zurück ist. Sonst zeichnet das Panel
  den Rest seines Frames aus freigegebenem Speicher. Siehe
  `MeshPaintPanel::SlotEdit`.
- **Der Undo-Push ist das Letzte im Block**, nie in der Mitte.

Wo `redo()` mehr tut als zuweisen, bleibt `push` richtig: `RoadShapeCmd::redo`
ruft `setShape`, und das setzt `needsBuild` und regeneriert Seitenobjekte, Stadt
und Decals. Gilt genauso für Spline und UI-Overlay.

---

## 4. `rotation` ist keine Blickrichtung

`Entity.rotation` ist ein Euler-Tripel, das als **Rz · Ry · Rx** komponiert wird.
Roll dreht also um die **Welt-Z-Achse**, nicht um die Nase des Objekts.

Für alles, was rollt, ist das Tripel nur noch „was zufällig die gewünschte Lage
ergibt". `rotation.y` ist dann **nicht** die Blickrichtung: ein Gleiter, der mit
22° Querlage nach Osten fliegt, speichert `y = 68`.

- Lage aus Gier/Nick/Roll bauen: `attitudeEuler()`
- Blickrichtung aus einem Tripel lesen: `sceneHeading()`

Beide in `SandboxMath.hpp`, wo auch steht, was schiefgeht, wenn man es von Hand
macht — und warum das Auto davon nie betroffen war und die Gleiter schon.

---

## 5. Bytes kommen aus `vfs`

Der Editor liest von der Platte, ein **exportiertes** Spiel aus einem
verschlüsselten `.fpak` neben der exe. `fitzel::vfs::read()` ist die eine Stelle,
die den Unterschied kennt.

**Ein neuer Ladepfad, der `ifstream` oder `fopen` direkt benutzt, funktioniert im
Editor tadellos und findet im Export nichts.** Genau deshalb fällt es nicht beim
Entwickeln auf, sondern beim Ausliefern.

Siehe `engine/include/fitzel/asset/Vfs.hpp`.

---

## 6. Parallele Arrays halten sich gegenseitig

`EditMesh::verts` und `EditMesh::paint` sind **eine** Sache in zwei Arrays: eine
Ecke und ihre Malgewichte. Jede Operation, die eine Ecke hinzufügt oder entfernt,
muss beide bewegen — `extrude`, `subdivide`, `deleteFace`. Alles, was Ecken nur
*verschiebt*, darf sie in Ruhe lassen.

Läuft es auseinander, zeichnet das Mesh weiterhin einwandfrei und die Farbe
taucht ein paar Striche später auf den falschen Faces auf, lange nach der
Operation, die es verursacht hat. Deshalb geht jedes Wachsen durch `addVert()`
und deshalb misst `meshpaintcheck` genau das.

---

## Was hier hineingehört

Eine Regel gehört auf diese Seite, wenn beides zutrifft:

- Sie gilt **zwischen** Dateien — wer sie bricht, liest die erklärende Datei nicht.
- Ihr Bruch ist **still** — kein Absturz, keine Meldung, nur ein Effekt, der
  ausbleibt oder woanders auftaucht.

Alles andere gehört als Kommentar an die Stelle, an der es gilt. Diese Seite lebt
nur, solange sie kurz genug bleibt, um gelesen zu werden.
