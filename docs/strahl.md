# Strahl — die Szenensprache des Pathtracers

Strahl beschreibt Szenen **für den Pathtracer**, so wie POV-Ray-Dateien Szenen für POV-Ray
beschreiben: Körper, Materialien, Licht und Kamera als Text. Anders als die geerntete
Spielszene (Render-Panel) besteht eine Strahl-Szene nicht aus Dreiecken, sondern aus
**exakten Körpern** — eine Kugel ist eine Kugel, eine Ebene geht bis zum Horizont, ein
Loch in einem Würfel ist ein echter Zylinder — kombiniert durch **CSG**, mit
**prozeduralen Mustern** und **Isoflächen**, die einfach Funktionen des Punktes sind.

```strahl
render { width: 1280, height: 720, samples: 256 }
camera { position: [0, 1.6, 5], look_at: [0, 0.7, 0], fov: 40 }
sun    { direction: [-1, 2, 1], intensity: 3, angle: 1.5 }
sky    { zenith: #4a6fa5, horizon: #cdd9e5 }

plane { material: matte(checker(#ddd, #333, size: 0.5)) }

difference {
    box    { size: 1.2 }
    sphere { radius: 0.78 }
    material: gold
    rotate: [0, 30, 0]
    translate: [0, 0.6, 0]
}

for i in 0..12 {
    sphere { center: [2.4 * cos(i * 30), 0.3, 2.4 * sin(i * 30)], radius: 0.3,
             material: glass(hsv(i * 30, 0.35, 1)) }
}
```

Alles hier ist im Code vorhanden und wird von `strahlcheck` geprüft
(`sandbox/src/Strahl*.cpp`, `TraceShapes.cpp`, `TraceFunc.cpp`): Jede eingebaute Funktion,
jede Objektart, jede Eigenschaft, jedes Schlüsselwort und jeder vordefinierte Wert hat hier
eine eigene Tabellenzeile — die Zeile, auf die F1 im Strahl-Fenster springt.

Diese Referenz steht im Editor unter **Help > Strahl Reference**.

---

## 1. Starten

### 1.1 Im Editor

**View > Presentation > Strahl (scene script)**. Links das Skript, rechts das Bild. Einen
Moment nach dem Tippen läuft das Skript erneut (in einem eigenen Thread) und ein Entwurf
(640 px, wenige Samples) baut sich auf — **Follow typing** schaltet das ab.

| Taste / Knopf | Wirkung |
| ------------- | ------- |
| **New** | ein neues Skript aus der Vorlage |
| **Open...** | Beispiele, die Skripte des Projekts, oder eine beliebige Datei |
| **Save** / Strg+S | Skript speichern (neue Skripte und Beispiele landen in `<Projekt>/strahl/`) |
| **Render** / Strg+Enter | volle Qualität mit den Einstellungen aus `render { }` |
| **Preview** / F5 | Entwurf jetzt rechnen |
| **Stop** | Skript und Bild abbrechen |
| **Save image** | PNG + lineares `.exr` nach `<Projekt>/renders/` |
| **Help** / F1 | diese Referenz; F1 schlägt den Namen am Cursor nach |

Ein Fehler unterstreicht die Stelle im Skript und steht rot unter dem Bild (anklicken
springt hin); das Bild bleibt das letzte, das gebaut hat. `print()` erscheint unter
**Output**. `sandbox --strahl datei.strahl` öffnet den Editor gleich mit einem Skript.

Gerechnet wird auf allen Kernen bis auf einen (`render { threads }` ändert das), im Entwurf
wie im vollen Bild.

### 1.2 Die Kamera im Bild bewegen

Im Vorschaubild steuert die Maus die Kamera; ein kleines schnelles Bild folgt ihr, und beim
Loslassen (oder wenn das Rad stillsteht) werden `position`, `look_at` (bzw. `direction`)
und, wenn geändert, `fov` in den `camera { }` des Skripts geschrieben — als ein Schritt, den
Strg+Z zurücknimmt — und das Skript läuft neu. Fehlt ein Wert, wird er ergänzt; hat das
Skript keine Kamera, kommt eine hinter `render { }`. Ein berechneter Wert
(`[5 * sin(time * 360), 2, 5]`, eine Variable) wird nicht überschrieben: Das Bild zeigt die
bewegte Kamera, unter ihm steht, warum das Skript unverändert blieb.

| Maus im Bild | |
| ------------ | --- |
| links ziehen | um den `look_at`-Punkt drehen |
| rechts ziehen | umschauen (der Punkt wandert mit) |
| Mitte ziehen / Umschalt+links | verschieben |
| Rad | näher heran / weiter weg |
| Strg+Rad | Bildwinkel (`fov`) |

### 1.3 Vervollständigung und Hilfe

Beim Tippen eines Namens klappt eine Liste auf, mit dem, was an dieser Stelle passt, zuerst:

* in `sphere { … }` die Eigenschaften der Kugel (die unbenannten vorn), im Block einer
  Variablen die ihrer Art (`let s = cylinder {…}` → `s { op` bietet `open`);
* nach `material:` die Materialien, nach `color:` Farben und Muster, nach `bump:` die
  Bump-Maps, nach `open:` `true`/`false`, nach `tonemap:` die Namen;
* in einem Aufruf die Parameter mit Namen (`marble(sc` → `scale: `) und darüber die
  Signatur, der aktuelle Parameter hervorgehoben;
* am Anfang einer Anweisung Schlüsselwörter und Objektarten; überall die eigenen `let`,
  `fn`, Parameter und Schleifenvariablen, die der importierten Dateien, der Bibliothek und
  die eingebauten Funktionen; in `import "` die Dateien.

Neben dem markierten Eintrag steht seine Beschreibung — für Funktionen der Bibliothek und
eigene ist das der Kommentar direkt über ihrem `fn`/`let`. Nach `{ `, `,` in einem Objekt
und nach `material: ` öffnet die Liste von selbst, wenn der Ort Eigenes anzubieten hat.
Der Wortteil muss nicht am Anfang stehen: `box` findet `rounded_box`, `rds` `radius`.

| Taste | |
| ----- | --- |
| ↑ ↓ Bild↑ Bild↓ | wählen |
| Tab / Enter | einsetzen (Objekte mit `{  }`, Funktionen mit `()`, Eigenschaften mit `: `) |
| Esc | schließen |
| Strg+Leertaste | Liste anfordern, auch ohne getippten Wortanfang |
| Strg+Umschalt+Leertaste | Signatur des Aufrufs zeigen |
| F1 | den Namen am Cursor in dieser Referenz nachschlagen |

**F1** öffnet die Referenz mit dem Namen als Suche, im Kapitel, dessen Tabelle ihn
beschreibt, und rollt zu seiner Zeile. Steht hinter dem Namen eine `(`, ist die Funktion
gemeint: `clamp(` landet bei der Funktion, `clamp:` in `render { }` bei der Eigenschaft.

### 1.4 Kommandozeile

```bash
build/linux/bin/strahl szene.strahl
```

| Option | |
| ------ | --- |
| `-o bild.png` | Ziel (sonst `render { output }` oder `szene.png` neben dem Skript) |
| `-s N`, `-w N`, `-h N`, `-b N` | Samples, Breite, Höhe, Bounces überschreiben |
| `-t N` | Render-Threads (sonst alle Kerne bis auf einen) |
| `-D name=wert` | ein `let` auf oberster Ebene vorbelegen (ein Strahl-Ausdruck: `-D farbe=#f00`) |
| `--frames N` | Animation: N Bilder, `time` läuft 0..1, nummerierte PNGs |
| `--frame K` | nur Bild K einer Animation |
| `--exr` | zusätzlich das lineare Bild |
| `--show base\|normal\|depth` | Diagnose statt Render |
| `--check` | nur bauen und berichten |
| `-L ordner` | weiterer Bibliotheksordner für `import` |
| `-q` | ohne Fortschrittsanzeige |

### 1.5 Beispiele und Bibliothek

Die Beispiele liegen in `sandbox/strahl/examples/` (beim Build nach `bin/strahl-lib/examples/`
kopiert) und öffnen sich im Editor unter **Open... > Examples**:

| Beispiel | zeigt |
| -------- | ----- |
| basics | erste Schritte: Körper, Material, Licht und Kamera |
| csg | `union`, `intersection`, `difference`, `merge` |
| materials | zwölf Kugeln, jede ein Muster — Farbe, Rauheit, Metall, Leuchten und Bump als Funktionen |
| isosurfaces | Formeln als Körper, mit den exakten Körpern per CSG verbunden |
| glass | ein Diamant nur aus Ebenen, gedrehtes Glas aus einem Profil, farbiges Glas über Absorption |
| city | eine Stadt in der Dämmerung aus einer Schleife, die Fenster ein Muster des Punktes |
| turntable | eine Animation: die Kamera einmal herum (`--frames`) |

`strahl-lib/studio.strahl` ist Bibliothek: `import "studio"` holt Hohlkehle und Studiolicht
(siehe 4.5).

---

## 2. Grundlagen

**Einheiten:** Meter. **Achsen:** rechtshändig, y nach oben (wie die Engine). **Winkel:
immer Grad** — in `rotate`, `fov`, und auch in `sin`, `cos`, `atan2` & Co.
(`sin(90) == 1`). `radians()` und `degrees()` rechnen um.

### 2.1 Werte

| Wert | Schreibweise |
| ---- | ------------ |
| Zahl | `1`, `0.5`, `.5`, `1e-3` |
| Vektor | `[x, y]`, `[x, y, z]`, `[x, y, z, w]` — zwei bis vier Zahlen in eckigen Klammern |
| Farbe | `#f80`, `#ff8800` (sRGB, wie ein Farbwähler), oder `rgb(1, 0.5, 0)`, `hsv(30, 1, 1)`, `gray(0.5)`, `kelvin(3200)` |
| Text | `"..."` oder `'...'`, mit `\n`, `\t` |
| Liste | `[a, b, c]` — alles andere als 2–4 Zahlen; Listen sind geteilt und veränderbar (`push`) |
| Bereich | `0..10` (ohne 10), `0..=10` (mit), `range(0, 1, 0.25)` |
| sonst | `true`, `false`, `nil`, Funktionen, Objekte, Matrizen, Bilder |

Vektoren rechnen komponentenweise: `[1, 2, 3] * 2 + 1 == [3, 5, 7]`; eine Zahl gilt für jede
Komponente. Komponenten: `.x .y .z .w` bzw. `.r .g .b .a`, beliebig kombiniert (`p.xz`,
`c.bgr`), oder `v[0]`, `v[-1]`.

### 2.2 Operatoren und Kommentare

Operatoren, schwach bindend zuerst: `or ||` · `and &&` · `not` · `== != < <= > >=` ·
`.. ..=` · `+ - ++` · `* / %` · unäres `- !` · `^` (Potenz, rechtsassoziativ). `++` hängt
Listen und Texte aneinander, `+` mit einem Text ebenfalls. `%` ist immer positiv.

Kommentare: `// bis Zeilenende` und `/* ... */`. Anweisungen brauchen kein Trennzeichen;
`;` und `,` sind überall erlaubt, wo eine Anweisung endet. Eine `(` oder `[` am
**Zeilenanfang** beginnt einen neuen Ausdruck, statt den der vorigen Zeile aufzurufen.

### 2.3 Namen, Funktionen, Ablauf

```strahl
let r = 0.5                       // Name binden
r = r * 2                         // neu zuweisen (auch += -= *= /=)

fn ring(n, radius = 2) {          // Funktion mit Vorgabewert
    for i in 0..n {
        sphere { center: [radius * cos(i * 360 / n), 0.2, radius * sin(i * 360 / n)], radius: 0.2 }
    }
}
fn sq(x) = x * x                  // Kurzform
let twice = fn(x) => x * 2        // anonyme Funktion (Closure)

ring(8)                           // Aufruf
ring(radius: 3, n: 12)            // benannte Argumente
```

Ein Block liefert den Wert seiner letzten Zeile — so geben Funktionen ohne `return` ihr
Ergebnis zurück. Rekursion geht (bis Tiefe 200).

### 2.4 Schlüsselwörter

| Wort | |
| ---- | --- |
| `let` | einen Namen binden: `let r = 0.5` |
| `fn` | eine Funktion: `fn name(a, b = 1) { … }`, `fn name(x) = …`, anonym `fn(x) => …` |
| `if` | Bedingung, auch als Ausdruck: `let t = if a > 1 { "groß" } else { "klein" }` |
| `else` | der andere Zweig von `if`, auch `else if` |
| `for` | Schleife: `for x in liste/bereich/vektor`, `for i, x in liste` (mit Index) |
| `in` | in `for`: was durchlaufen wird |
| `while` | Schleife, solange die Bedingung gilt: `while bedingung { }` |
| `return` | eine Funktion mit diesem Wert verlassen |
| `break` | die Schleife verlassen |
| `continue` | weiter mit dem nächsten Durchlauf |
| `import` | eine Datei einmal ausführen, ihre `let` und `fn` übernehmen (2.5) |
| `true` | wahr |
| `false` | falsch |
| `nil` | kein Wert |
| `and` | logisches Und, auch `&&` |
| `or` | logisches Oder, auch `\|\|` |
| `not` | Verneinung, auch `!` |

### 2.5 import

`import "datei"` (`.strahl` darf fehlen) führt eine Datei einmal aus; ihre `let` und `fn`
stehen danach zur Verfügung. Gesucht wird neben dem Skript, dann in den Ordnern aus `-L`, dann in
der Bibliothek (`strahl-lib/`, z. B. `import "studio"` für Hohlkehle und Studiolicht).

---

## 3. Objekte

### 3.1 Schreibweisen

Ein Objekt ist eine Art und ein Block von Eigenschaften:

```strahl
sphere { center: [0, 1, 0], radius: 0.5, material: gold }
sphere([0, 1, 0], 0.5, material: gold)       // dasselbe als Aufruf
sphere { [0, 1, 0], 0.5, gold }               // unbenannt, in der Reihenfolge der Art
```

Unbenannte Werte füllen die Parameter der Art der Reihe nach (bei `sphere`: `center`,
`radius`); ein Material darunter ist das Material, wo immer es steht. Benannte Werte
gewinnen gegen unbenannte, spätere gegen frühere.

### 3.2 Ändern heißt kopieren

Ein Block hinter einem vorhandenen Objekt macht eine geänderte Kopie; das Original bleibt:

```strahl
let saeule = cylinder { from: [0, 0, 0], to: [0, 3, 0], radius: 0.2 }
saeule { translate: [-2, 0, 0], material: marble() }
saeule { translate: [ 2, 0, 0], material: granite() }
```

Das gilt auch für das Ergebnis einer Funktion: `ring(8) { material: chrome, translate: [0, 1, 0] }`.

### 3.3 Was jedes Objekt nimmt

Transformationen wirken in der Reihenfolge, in der sie dastehen, und jede nach den vorigen.
Körper, Gruppen, Lichter, Sonne und Kamera lassen sich alle so bewegen.

| Eigenschaft | Bedeutung |
| ----------- | --------- |
| `material` | das Material eines Körpers: ein `material { }`, eine Farbe (Kunststoff) oder ein Muster (Kapitel 5) |
| `translate` | verschieben um `[x, y, z]` |
| `rotate` | drehen um `[x°, y°, z°]` — erst um x, dann y, dann z |
| `scale` | skalieren: `2` oder `[sx, sy, sz]` (auch ungleichmäßig) |
| `matrix` | eine Matrix anwenden, aus `translation()`, `rotation()`, `rotation_axis()`, `scaling()` (multiplizierbar; `m * p` transformiert einen Punkt) |
| `transform` | dasselbe wie `matrix` |

### 3.4 Wohin Objekte gehen

* Eine Anweisung, deren Wert ein Objekt ist, legt es in die Szene.
* In den `{ }` eines Objekts werden Objekte zu seinen **Kindern** (CSG-Operanden,
  Gruppenmitglieder), andere Werte zu seinen unbenannten Parametern.
* In einer Funktion werden erzeugte Objekte gesammelt und **zurückgegeben** — eins als es
  selbst, mehrere als `group`. Darum verhält sich eine Funktion wie ein Konstruktor.
* Was nur berechnet wird — rechts von `let`, als Argument, ein `if` als Wert — legt nichts
  ab. `let k = sphere { }` erzeugt die Kugel, zeigt sie aber nicht.

---

## 4. Körper

### 4.1 Grundkörper

Alle Körper nehmen `material`, `translate`, `rotate`, `scale`, `matrix` (3.3). Fett: die
unbenannten Parameter in ihrer Reihenfolge.

| Art | Eigenschaften | |
| --- | ------------- | --- |
| `sphere` | **center**, **radius** | eine Kugel |
| `box` | **min**, **max** — oder `size` und `center` | ein achsenparalleler Quader; ohne Angaben ein Würfel von 1 m um den Ursprung |
| `plane` | **normal**, **offset** — oder `point` | eine unendliche Ebene; fest ist die Seite, von der die Normale wegzeigt |
| `disc` | **center**, **normal**, **radius**, `hole` | eine flache Scheibe ohne Volumen (nicht in CSG sinnvoll) |
| `cylinder` | **from**, **to**, **radius**, `open` | ein Zylinder entlang der Achse von `from` nach `to` |
| `cone` | **from**, **to**, **radius**, **top_radius**, `open` | ein Kegelstumpf: `radius` bei `from`, `top_radius` bei `to` |
| `torus` | **major**, **minor** | ein Ring in der x-z-Ebene |
| `isosurface` | **function**, `bound` / `min`, `max`, `threshold`, `max_gradient`, `accuracy`, `open` | die Fläche, auf der eine Funktion den Schwellwert hat (4.3) |
| `mesh` | **vertices**, **faces**, `normals`, `uvs`, `smooth` | Dreiecke/Polygone aus Listen; nicht in CSG |

### 4.2 Eigenschaften der Körper

| Eigenschaft | bei | Bedeutung |
| ----------- | --- | --------- |
| `center` | sphere, box, disc | der Mittelpunkt `[x, y, z]` (Vorgabe: der Ursprung) |
| `radius` | sphere, disc, cylinder, cone | der Radius in Metern |
| `min` | box, isosurface | die kleinste Ecke `[x, y, z]` |
| `max` | box, isosurface | die größte Ecke `[x, y, z]` |
| `size` | box | die Kantenlänge: eine Zahl oder `[x, y, z]` |
| `normal` | plane, disc | die Richtung, in die die Fläche zeigt |
| `offset` | plane | der Abstand der Ebene vom Ursprung, entlang der Normalen |
| `point` | plane | ein Punkt, durch den die Ebene geht (statt `offset`) |
| `hole` | disc | der Radius des Lochs in der Mitte |
| `from` | cylinder, cone | ein Ende der Achse |
| `to` | cylinder, cone | das andere Ende der Achse |
| `open` | cylinder, cone, isosurface | `true`: ohne Deckel (bei der Isofläche: wo die Box sie schneidet, bleibt sie offen) |
| `top_radius` | cone | der Radius bei `to` (0: eine Spitze) |
| `major` | torus | der Radius des Rings |
| `minor` | torus | der Radius des Rohrs |
| `vertices` | mesh | die Punkte: eine Liste von `[x, y, z]` |
| `faces` | mesh | die Polygone: Listen von Punkt-Indizes, von außen gesehen gegen den Uhrzeigersinn |
| `normals` | mesh | eine Normale pro Punkt (sonst berechnet) |
| `uvs` | mesh | ein `[u, v]` pro Punkt |
| `smooth` | mesh | `true`: Normalen über die Flächen gemittelt |

### 4.3 Isoflächen

```strahl
isosurface {
    function: fn(p) => length(p) - 0.7 + 0.15 * noise(p * 4)   // < 0 innen
    bound: 1              // Würfel ±1 (oder [x, y, z], oder min/max)
    threshold: 0
    max_gradient: 2       // wenn weggelassen: gemessen (steht im Log)
}
```

| Eigenschaft | Bedeutung |
| ----------- | --------- |
| `function` | `fn(p) => …`: unter dem Schwellwert innen, darüber außen; wird für den Tracer übersetzt (6.1) |
| `bound` | die Box um den Ursprung, in der gesucht wird: eine halbe Kantenlänge, Zahl oder `[x, y, z]` (oder `min`/`max`) |
| `threshold` | der Wert der Funktion auf der Fläche (0) |
| `max_gradient` | die steilste Steigung der Funktion; weggelassen wird sie gemessen und ins Log geschrieben |
| `accuracy` | wie nah die Suche an die Fläche herankommt, in Metern |

Die Fläche wird abgetastet: Schritte so groß, wie `|f| / max_gradient` erlaubt, an einem
Vorzeichenwechsel per Bisektion verfeinert. Zu kleines `max_gradient` lässt dünne Stellen
aus (Löcher), zu großes kostet Zeit. Wo die Box den Körper schneidet, schließt sie ihn
(außer `open: true`). Isoflächen sind volle CSG-Operanden.

### 4.4 CSG

| Art | Ergebnis |
| --- | -------- |
| `union` | alles zusammen: `union { a b ... }` (auf oberster Ebene einfach eine Gruppe) |
| `merge` | wie `union`, aber ohne innere Flächen — für Glas |
| `intersection` | nur was in allen liegt |
| `difference` | das erste Kind ohne alle anderen: `difference { a b c }` = `a` ohne `b` und ohne `c` |
| `group` | Kinder nur gemeinsam bewegen und färben |
| `object` | dasselbe wie `group` |

Operanden ohne eigenes Material erben das der Kombination. CSG ist exakt: der Strahl läuft
die Kreuzungen der Operanden entlang und behält die, die die Regel erfüllen. Ebenen sind
gute Operanden (`intersection` aus Ebenen = Polyeder, siehe den Diamanten in
`examples/glass.strahl`). Flächen, die genau aufeinanderliegen, sind wie in jedem
CSG-Raytracer zweideutig — einen Hauch überstehen lassen.

### 4.5 Körper aus der Bibliothek

In Strahl geschrieben (`sandbox/src/StrahlPrelude.cpp`, `strahl-lib/studio.strahl`):

| Funktion | |
| -------- | --- |
| `rounded_box(size = 1, radius = 0.1, center = [0, 0, 0])` | ein Quader, dessen Kanten um `radius` gerundet sind (exakte Distanzfunktion) |
| `capsule(from = [0, 0, 0], to = [0, 1, 0], radius = 0.25)` | ein Zylinder mit einer Halbkugel an jedem Ende |
| `blob(centers, radius = 0.5, threshold = 0.4)` | Metaballs: Einflusskugeln um die Punkte `centers`, die ineinanderfließen |
| `lathe(points, segments = 64)` | ein Rotationskörper: `points` ist das Profil, `[radius, höhe]`-Paare von unten nach oben, um die y-Achse gedreht |
| `heightfield(function, size = [10, 10], resolution = 128)` | eine Fläche `y = function(x, z)` über `size` Metern `[breite, tiefe]` um den Ursprung, `resolution` Zellen breit |
| `cyclorama(width = 16, depth = 10, height = 7, radius = 2.5, color = #e6e4e0, segments = 24)` | Hohlkehle: ein Boden, der ohne Ecke in die Wand übergeht (`import "studio"`) |
| `studio_lights(key = 70, fill = 14, rim = 45, warmth = 5000)` | drei weiche Lampen: Führungslicht links oben, kühleres Aufhelllicht rechts, Kante von hinten; `warmth` in Kelvin (`import "studio"`) |

---

## 5. Materialien

### 5.1 material { }

```strahl
material {
    color: #c83a2c          // Farbe (sRGB) — oder ein Muster
    roughness: 0.3          // 0 Spiegel .. 1 matt — oder ein Muster
    metallic: 0             // 0 Kunststoff .. 1 Metall — oder ein Muster
    emission: #ffd080       // Leuchten (sRGB) — oder ein Muster
    strength: 4             // Faktor für emission
    glass: true             // Dielektrikum mit Brechung
    ior: 1.5                // Brechungsindex (setzt glass, wenn allein)
    absorption: [0.1, 0.5, 1.2]  // Absorption pro Meter im Glas: Farbe aus Tiefe
    bump: fn(p) => ...      // Höhenfeld in Metern, kippt die Normale
}
```

| Art / Eigenschaft | Bedeutung |
| ----------------- | --------- |
| `material` | eine Oberfläche: Farbe, Rauheit, Metall, Glas, Leuchten, Bump. Jeder Kanal kann ein Muster sein |
| `color` | die Farbe (sRGB) — oder ein Muster. Bei Lichtern, Sonne und Nebel: deren Farbe |
| `roughness` | 0 ein Spiegel .. 1 matt — oder ein Muster |
| `metallic` | 0 Kunststoff/Dielektrikum .. 1 Metall — oder ein Muster |
| `glass` | `true`: ein Dielektrikum, das bricht |
| `ior` | der Brechungsindex: Wasser 1.33, Glas 1.5, Diamant 2.42; setzt `glass`, wenn allein angegeben |
| `absorption` | Licht, das pro Meter im Glas verloren geht, je Kanal: Farbe, die mit der Dicke satter wird |
| `medium_color` | die Eigenfarbe des Glaskörpers (Wasser) |
| `emission` | die Farbe, in der die Fläche leuchtet (sRGB), oder ein Muster |
| `strength` | der Faktor für `emission` |
| `translucency` | der Anteil des Lichts, der auf der Rückseite austritt (Blatt, Papier) |
| `opacity` | unter 1: teilweise durchsichtig |
| `bump` | eine Höhe in Metern pro Punkt — `fn(p) => …` oder `bumps()`, `dents()`, … (6.3) — die die Normale kippt |
| `bump_strength` | der Faktor für `bump` |
| `shadow` | `false`: wirft keinen Schatten |
| `additive` | `true`: Licht wird hinzugezählt statt verdeckt (Gischt, Flammen) |

`translate` / `rotate` / `scale` in einem Material verschieben das Muster gegenüber dem
Körper. Statt eines Materials geht auch eine Farbe (`material: #f00`, ein Kunststoff) oder
ein Muster (`material: marble()`).

### 5.2 Ausgangspunkt und Kopie

Ein Material ohne Namen in den `{ }` eines Materials ist sein **Ausgangspunkt**, wie
`texture { T_Stone … }` in POV-Ray: `material { gold, roughness: 0.3 }` ist Gold mit
anderer Rauheit, `material { matte(checker(#ddd, #555, size: 0.5)) }` das matte Schachbrett.
Was im Block steht, gewinnt; eine Farbe ohne Namen dahinter ersetzt die Farbe. Dasselbe
geht als Kopie: `gold { roughness: 0.3 }`.

Farbiges Glas färbt man am besten über `absorption` (pro Meter verlorenes Licht je Kanal):
dünnes Glas bleibt hell, dickes wird satt. `color` tönt dagegen jeden Durchgang gleich.

**Muster leben im Raum des Objekts**, dem das Material gegeben wurde, vor dessen eigenen
Transformationen — ein Marmorblock behält seine Adern, wenn er bewegt wird, und ein
`difference` zeigt im Schnitt den Marmor von innen.

### 5.3 Vorlagen

| Funktion | |
| -------- | --- |
| `matte(color = #bfbfbf)` | eine stumpfe Fläche: Kreide, Papier, unglasierter Ton |
| `plastic(color = #bfbfbf, roughness = 0.3)` | farbiger Kunststoff mit Klarlack; `roughness` 0 glänzt wie ein Spiegel |
| `metal(color = #c8c8cc, roughness = 0.12)` | ein Metall: `color` tönt die Spiegelung, `roughness` verwischt sie |
| `glass(color = white, ior = 1.5, absorption = 0)` | Glas mit Brechungsindex und Absorption |
| `emissive(color = white, strength = 5)` | eine leuchtende Fläche: beleuchtet die Szene nur über Abpraller (ein `light { }` mit `radius` ist die rauschärmere Lampe) |

### 5.4 Fertige Materialien

| Name | ist |
| ---- | --- |
| `chrome` | `metal(#e2e2e6, 0.02)` |
| `mirror` | `metal(white, 0)` |
| `silver` | `metal(#fbf8f2, 0.08)` |
| `gold` | `metal(#ffd890, 0.12)` |
| `copper` | `metal(#fad0c0, 0.16)` |
| `brass` | `metal(#f4e2aa, 0.2)` |
| `steel` | `metal(#c4c6c8, 0.25)` |
| `aluminium` | `metal(#f4f4f4, 0.3)` |
| `water` | `glass(white, 1.33)` |
| `crystal` | `glass(white, 1.55)` |
| `diamond` | `glass(white, 2.42)` |

---

## 6. Muster und Bump-Maps

### 6.1 Funktionen pro Strahl

Ein Muster ist eine Funktion des Punktes: `fn(p) => ...`, optional `fn(p, n, uv)` mit der
Weltnormalen `n` und den Flächenkoordinaten `uv`. Zurück kommt eine Farbe (oder eine Zahl
für roughness, metallic, bump, isosurface).

```strahl
material { color: fn(p) => mix(#203060, #e0e8ff, smoothstep(-0.3, 0.3, noise(p * 4))) }
```

Solche Funktionen werden **beim Bauen in Bytecode übersetzt** (`TraceFunc.hpp`) und laufen
pro Strahl ohne den Interpreter. Dabei wird alles, was nicht vom Punkt abhängt, sofort
ausgerechnet — Aufrufe, die Funktionen liefern, Farblisten, Bilder. Übrig bleibt die
Arithmetik auf dem Punkt.

* Pro Strahl erlaubt: Zahlen und Vektoren bis 3 Komponenten, `let`, Zuweisung, `if`,
  `return`, Aufrufe (werden eingesetzt), `for` über **vorher bekannte** Bereiche/Listen
  (wird ausgerollt, höchstens 256 Durchläufe).
* Nicht erlaubt (mit Meldung): `while`, Rekursion, Listen oder Objekte bauen, Funktionen
  definieren.

Bausteine sind alle Mathefunktionen (9.1), die Vektorfunktionen (9.2), `mix`, `ramp`, `at`
und das Rauschen (9.4); `texture()` liest ein Bild aus `load_image()`.

### 6.2 Fertige Muster

Ihre Farbargumente dürfen selbst Muster sein: `checker(marble(), wood(), size: 0.25)`.

| Funktion | |
| -------- | --- |
| `checker(a = white, b = black, size = 1)` | Farben oder Muster als 3D-Schachbrett mit Zellen von `size` Metern |
| `stripes(a = white, b = black, width = 0.5, axis = [1, 0, 0])` | Streifen quer zu `axis`, je `width` breit |
| `rings(a = white, b = black, width = 0.25)` | Ringe um die y-Achse |
| `gradient(a = black, b = white, from = 0, to = 1, axis = [0, 1, 0])` | von `a` nach `b` entlang `axis`, zwischen den Abständen `from` und `to` |
| `clouds(a = white, b = #7a8ca0, scale = 1, octaves = 5)` | eine wolkige Mischung zweier Farben |
| `marble(base = #ebe6dd, vein = #46474e, scale = 0.5, swirl = 2.5, sharpness = 6)` | Marmor: dünne dunkle Adern, die durch hellen Stein wandern |
| `wood(light = #d6a66c, dark = #8a582e, scale = 0.04, swirl = 0.5)` | Holz: Jahresringe um die y-Achse, gewellt, mit Maserung entlang |
| `granite(a = #c2bab0, b = #2e2c2b, c = #8e7f73, scale = 0.012)` | Granit: gesprenkelte Körner in fleckigem Grund |
| `spots(a = white, b = black, size = 0.2, amount = 0.35)` | runde Flecken von `b` auf `a`, etwa `size` groß |
| `bricks(brick = #9c4a34, mortar = #d4cbbd, size = [0.24, 0.075], gap = 0.01)` | eine Ziegelwand in der x-y-Ebene (für andere Wände das Material drehen): Läuferverband, `size` = `[länge, höhe]`, Fuge `gap`, jeder Ziegel etwas anders |
| `tiles(a = #e6e1d8, b = #e6e1d8, grout = #8a847a, size = 0.3, gap = 0.006)` | quadratische Fliesen auf dem Boden (x-z-Ebene), abwechselnd `a` und `b`, mit Fugen |
| `image(file, scale = [1, 1])` | ein Bild, nach den uv der Fläche aufgelegt (einer Kugel, einer Quaderseite, eines Meshes); `scale` wiederholt es |
| `planar(file, size = 1)` | ein Bild senkrecht von oben auf die x-z-Ebene projiziert, `size` Meter je Kachel |

### 6.3 Bump-Maps

Höhe in Metern. Die Normale wird um den Gradienten gekippt; die Geometrie bleibt. Statt
einer fertigen geht jede eigene Funktion `fn(p) => …`.

| Funktion | |
| -------- | --- |
| `bumps(amount = 0.004, scale = 0.05)` | weiche Rauschbeulen, `scale` Meter groß: gehämmertes Metall, Orangenhaut |
| `wrinkles(amount = 0.002, scale = 0.04)` | scharfe Knicke: zerknitterte Folie, Leder |
| `dents(amount = 0.003, size = 0.05)` | runde Dellen, etwa `size` groß: getriebenes Kupfer, Golfball |
| `ripples(amount = 0.003, wavelength = 0.12, center = [0, 0, 0])` | Ringe, die sich von `center` ausbreiten, `wavelength` auseinander: ein Tropfen im Wasser |
| `waves(amount = 0.02, scale = 1)` | rollende Wellen über die x-z-Ebene, für eine Wasserfläche |

---

## 7. Licht, Himmel, Nebel

| Art | |
| --- | --- |
| `sun` | die Sonne, unendlich weit weg; höchstens eine pro Szene |
| `light` | ein Punktlicht; `intensity` ist die Beleuchtung in 1 m Abstand, sie fällt mit 1/d² |
| `spot` | ein Scheinwerfer bei `position`, auf `look_at` gerichtet (oder entlang `direction`) |
| `sky` | der Himmel: ein Verlauf, ein Panorama (`hdri`) oder der Himmel der Engine |
| `fog` | Höhennebel wie im Viewport |

Lichter und Sonne lassen sich wie Körper transformieren und in Gruppen bewegen.

### 7.1 sun

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `direction` | `[0.5, 1, 0.35]` | die Richtung **zur Sonne hin** |
| `color` | warmweiß | ihre Farbe |
| `intensity` | 3 | wie hell |
| `angle` | 0.53 | der Durchmesser der Sonnenscheibe in Grad; größer = weichere Schatten |

### 7.2 light

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `position` | `[0, 3, 0]` | wo es ist `[x, y, z]` |
| `color` | weiß | seine Farbe |
| `intensity` | 10 | die Beleuchtung in 1 m Abstand |
| `radius` | 0.05 | die Größe der Lampe: größer = weichere Schatten, weniger Rauschen |
| `range` | unbegrenzt | weiter weg beleuchtet es nichts mehr |
| `visible` | `false` | `true`: die Glühbirne selbst ist im Bild zu sehen (sie wirft kein zusätzliches Licht) |

### 7.3 spot

Wie `light`, dazu:

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `look_at` | | der Punkt, auf den er zeigt |
| `direction` | | die Richtung, in die er zeigt (statt `look_at`) |
| `angle` | 30 | der halbe Öffnungswinkel des Kegels in Grad |
| `inner` | 0.7 × `angle` | der halbe Winkel, in dem der Spot volle Helligkeit hat |
| `intensity` | 20 | wie hell |

### 7.4 sky

| Eigenschaft | Bedeutung |
| ----------- | --------- |
| `zenith` | die Farbe genau oben |
| `horizon` | die Farbe am Horizont |
| `ground` | die Farbe unter dem Horizont |
| `intensity` | wie hell (1) |
| `hdri` | eine Panoramadatei (`.hdr`, `.exr`), die die Szene beleuchtet und den Hintergrund füllt |
| `rotate` | das Panorama um die y-Achse drehen, in Grad |
| `procedural` | `true`: der eigene Himmel der Engine, mit Sonne |
| `background` | was die Kamera hinter allem sieht; das Licht kommt weiter vom Himmel |

`sky { background: farbe }` ist nichts für Glasszenen: auch Brechungsstrahlen sehen den
Hintergrund, und ein dunkler macht Glas und Diamanten schwarz.

### 7.5 fog

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `density` | 0.02 | wie dicht der Nebel ist |
| `color` | blassblau | seine Farbe |
| `height` | 0 | die Höhe, ab der gemessen wird |
| `falloff` | 0 | wie schnell er nach oben dünner wird |
| `sun_color` | warm | die Farbe des Scheins um die Sonne im Nebel |

### 7.6 Leuchtende Körper oder Lampen

Leuchtende Materialien (`emission`, `emissive()`) beleuchten die Szene nur über Abpraller —
für eine Lampe ist `light` mit `radius` die rauschärmere Wahl, mit `visible: true`, wenn die
Birne zu sehen sein soll.

---

## 8. Kamera und Render

### 8.1 camera

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `camera` | | die Kamera: wo sie steht, wohin sie schaut, Bildwinkel, Schärfentiefe |
| `position` | `[0, 1.5, 6]` | wo sie steht |
| `look_at` | `[0, 0, 0]` | der Punkt, auf den sie schaut |
| `direction` | | die Richtung, in die sie schaut (statt `look_at`) |
| `up` | `[0, 1, 0]` | wo oben ist |
| `fov` | 45 | der vertikale Bildwinkel in Grad |
| `aperture` | 0 | der Blendenradius in Metern: Schärfentiefe; 0 = alles scharf |
| `focus` | Abstand zu `look_at` | die scharfe Entfernung in Metern, oder `"auto"`: was die Bildmitte trifft |
| `roll` | 0 | die Kamera um die Blickrichtung kippen, in Grad |

### 8.2 render

| Eigenschaft | Vorgabe | Bedeutung |
| ----------- | ------- | --------- |
| `render` | | wie das Bild entsteht: Größe, Samples, Bounces, Tonemapping und Farbkorrektur |
| `width` | 1280 | die Bildbreite in Pixeln |
| `height` | 720 | die Bildhöhe in Pixeln |
| `samples` | 128 | Samples pro Pixel: mehr ist weniger Rauschen und mehr Zeit |
| `bounces` | 6 | wie oft ein Pfad abprallen darf |
| `batch` | 4 | Samples pro Durchgang zwischen zwei Fortschrittsanzeigen |
| `seed` | 1 | der Zufallswert: anderer Seed, anderes Rauschen |
| `threads` | 0 | Render-Threads, höchstens 64; 0 = alle Kerne bis auf einen |
| `clamp` | 24 | Obergrenze für das, was ein einzelner indirekter Pfad beitragen darf (gegen Glühwürmchen); 0 = aus |
| `exposure` | 1 | Belichtung als Faktor: 2 ist doppelt so hell |
| `tonemap` | `"aces"` | `"aces"`, `"agx"` oder `"neutral"` |
| `saturation` | 1 | Farbsättigung |
| `contrast` | 0 | Kontrast: 0 unverändert, darüber härter, darunter weicher |
| `warmth` | 0 | wärmer (+) oder kälter (−) |
| `hue` | 0 | die Farbtöne drehen, in Grad |
| `vignette` | 0 | die Ecken abdunkeln |
| `output` | | die Datei, in die die Kommandozeile das Bild schreibt |

### 8.3 Animation

`time` läuft über `--frames N` von 0 bis 1, `frame` zählt, `frames` ist N:

```strahl
camera { position: [5 * sin(time * 360), 2, 5 * cos(time * 360)], look_at: [0, 0.7, 0] }
```

```bash
build/linux/bin/strahl turntable.strahl --frames 120
```

| Name | |
| ---- | --- |
| `time` | wo in der Animation dieses Bild liegt: 0 beim ersten, 1 beim letzten (`--frames`) |
| `frame` | die Nummer des Bildes, das gerade entsteht |
| `frames` | wie viele Bilder die Animation hat |

---

## 9. Eingebaute Funktionen

### 9.1 Mathe

Winkel in Grad. Alle wirken komponentenweise auf Vektoren und lassen sich pro Strahl
übersetzen.

| Funktion | |
| -------- | --- |
| `sin(x)` | Sinus eines Winkels in Grad |
| `cos(x)` | Kosinus eines Winkels in Grad |
| `tan(x)` | Tangens eines Winkels in Grad |
| `asin(x)` | Arkussinus, in Grad |
| `acos(x)` | Arkuskosinus, in Grad |
| `atan(x)` | Arkustangens, in Grad |
| `atan2(y, x)` | der Winkel des Punktes (x, y) in Grad, −180..180 |
| `sqrt(x)` | Quadratwurzel (0 unter null) |
| `pow(x, y)` | x hoch y; der Operator `^` tut dasselbe |
| `exp(x)` | e hoch x |
| `log(x)` | natürlicher Logarithmus |
| `log2(x)` | Logarithmus zur Basis 2 |
| `log10(x)` | Logarithmus zur Basis 10 |
| `abs(x)` | Betrag |
| `sign(x)` | −1, 0 oder 1 |
| `floor(x)` | abgerundet |
| `ceil(x)` | aufgerundet |
| `round(x)` | auf die nächste ganze Zahl gerundet |
| `fract(x)` | der Teil hinter dem Komma: `x - floor(x)` |
| `mod(x, y)` | Rest von x / y, immer mit dem Vorzeichen von y (auch für negatives x) |
| `min(values…)` | das Kleinste — der Argumente oder einer Liste |
| `max(values…)` | das Größte — der Argumente oder einer Liste |
| `clamp(x, lo = 0, hi = 1)` | x zwischen lo und hi gehalten |
| `saturate(x)` | x zwischen 0 und 1 gehalten |
| `mix(a, b, t)` | von a (t = 0) nach b (t = 1); Zahlen, Vektoren und Farben |
| `lerp(a, b, t)` | dasselbe wie `mix()` |
| `smoothstep(edge0, edge1, x)` | 0 unter edge0, 1 über edge1, dazwischen ein weiches S |
| `step(edge, x)` | 0 unter edge, ab edge 1 |
| `radians(x)` | Grad nach Bogenmaß (die Trigonometrie hier nimmt Grad) |
| `degrees(x)` | Bogenmaß nach Grad |

### 9.2 Vektoren

| Funktion | |
| -------- | --- |
| `vec(values…)` | ein Vektor aus Zahlen und kleineren Vektoren: `vec(p.xz, 1)` |
| `length(v)` | die Länge eines Vektors |
| `normalize(v)` | der Vektor auf Länge 1 gebracht |
| `dot(a, b)` | Skalarprodukt |
| `cross(a, b)` | Kreuzprodukt zweier 3-Vektoren |
| `distance(a, b)` | der Abstand zweier Punkte |
| `reflect(d, n)` | die Richtung d an der Normalen n gespiegelt |

### 9.3 Farben

| Funktion | |
| -------- | --- |
| `rgb(r, g, b)` | eine Farbe aus linearen Anteilen 0..1 (eine `#hex`-Farbe ist sRGB) |
| `hsv(h, s = 1, v = 1)` | Farbton in Grad, Sättigung und Helligkeit 0..1 |
| `gray(v)` | ein Grau: `[v, v, v]` |
| `kelvin(k)` | die Farbe eines schwarzen Strahlers bei `k` Kelvin, normiert: 1800 eine Kerze, 2700 eine Glühbirne, 5500 Tageslicht, 7500 bedeckter Himmel |
| `ramp(t, colors)` | eine Farbe auf einem Verlauf: `[c0, c1, ...]` gleichmäßig verteilt, oder `[[0.3, c], ...]` an festen Stellen |
| `at(pattern, p, n, uv)` | eine Farbe oder ein Muster an einem Punkt: eine Farbe ist sie selbst, ein Muster wird gefragt. So schachteln sich Muster |

### 9.4 Rauschen und Zufall

| Funktion | |
| -------- | --- |
| `noise(p)` | Perlin-Rauschen an einem Punkt, etwa −1..1 |
| `fbm(p, octaves = 5)` | Rauschen über Oktaven summiert: Wolken, Gelände, etwa −1..1 |
| `turbulence(p, octaves = 5)` | summiertes `\|noise\|`: scharfe Falten, 0..1; Marmoradern, Feuer |
| `voronoi(p)` | der Abstand zum nächsten zufällig verstreuten Zellpunkt |
| `cell_id(p)` | eine Zahl 0..1, die über jede Voronoi-Zelle gleich bleibt |
| `hash(p)` | eine Zufallszahl 0..1 aus dem genauen Wert: gleiche Eingabe, gleiche Zahl |
| `parity(p)` | 0 oder 1 auf einem 3D-Schachbrett aus Einheitszellen |
| `random(seed, lo = 0, hi = 1)` | eine wiederholbare Zufallszahl zu einem Seed, ein Vektor davon zu einem Vektor-Seed |

### 9.5 Listen und Text

| Funktion | |
| -------- | --- |
| `list(values…)` | eine Liste der Argumente |
| `range(start, end, step = 1)` | die Zahlen von start bis (ohne) end; auch `start..end` |
| `len(value)` | die Zahl der Einträge einer Liste, Zeichen eines Textes, Komponenten eines Vektors |
| `push(list, value)` | an eine Liste anhängen (ändert sie) |
| `pop(list)` | den letzten Eintrag entfernen und zurückgeben |
| `map(list, f)` | eine neue Liste aus `f(eintrag)` für jeden Eintrag |
| `filter(list, f)` | die Einträge, für die `f(eintrag)` wahr ist |
| `reverse(list)` | die Liste rückwärts |
| `sum(list)` | alle Einträge addiert |
| `contains(list, value)` | ob die Liste den Wert enthält |
| `str(values…)` | die Werte als Text, aneinandergehängt |

### 9.6 Matrizen

| Funktion | |
| -------- | --- |
| `identity()` | die Einheitsmatrix |
| `translation(v)` | eine Matrix, die um v verschiebt |
| `rotation(degrees)` | eine Matrix, die um `[x, y, z]` Grad dreht, x zuerst (wie `rotate:`) |
| `rotation_axis(axis, degrees)` | eine Matrix, die um eine Achse dreht |
| `scaling(s)` | eine Matrix, die um eine Zahl oder einen Vektor skaliert |
| `inverse(m)` | die Inverse einer Matrix |
| `apply(m, point)` | ein Punkt durch die Matrix transformiert (dasselbe wie `m * point`) |
| `apply_dir(m, direction)` | eine Richtung durch die Matrix transformiert (ohne Verschiebung) |

### 9.7 Bilder, Ausgabe, Fehler

| Funktion | |
| -------- | --- |
| `load_image(file)` | ein Bild lesen (PNG, JPG, HDR, EXR) für `texture()`, neben dem Skript oder in der Bibliothek |
| `texture(image, uv)` | ein Bild aus `load_image()` bei uv (0..1, wiederholt) |
| `print(values…)` | ins Output-Fenster schreiben (und auf die Konsole) |
| `error(message)` | das Skript mit dieser Meldung an dieser Zeile anhalten |
| `assert(condition, message)` | das Skript anhalten, wenn die Bedingung falsch ist |
| `type(value)` | der Typ des Werts als Text: `"number"`, `"vector"`, `"object"`, … |

### 9.8 Konstanten und Farben

| Name | |
| ---- | --- |
| `pi` | eine halbe Drehung im Bogenmaß |
| `tau` | eine ganze Drehung im Bogenmaß |
| `e` | die Eulersche Zahl |
| `inf` | praktisch unendlich |
| `x` | die x-Achse `[1, 0, 0]` |
| `y` | die y-Achse `[0, 1, 0]`: oben |
| `z` | die z-Achse `[0, 0, 1]`: zur Vorgabe-Kamera hin |
| `white` | `#ffffff` |
| `black` | `#000000` |
| `red` | `#c8282a` |
| `green` | `#3a9a3c` |
| `blue` | `#2a56c8` |
| `yellow` | `#f2d02c` |
| `orange` | `#f08424` |
| `cyan` | `#2cc4d4` |
| `magenta` | `#c43a9c` |
| `purple` | `#74389c` |
| `brown` | `#7a4a28` |
| `pink` | `#f0a0b4` |
| `ivory` | `#f4eee0` |

---

## 10. Grenzen

* **Nur der CPU-Tracer** kennt exakte Körper und Musterfunktionen. Der GPU-Tracer (Viewport-
  Vorschau, `--shots-trace-gpu`) bekommt Strahl-Szenen nicht zu sehen.
* Keine Kaustiken: Licht durch Glas wird nicht gebündelt; der Schatten eines Glaskörpers ist
  ein getönter Halbschatten.
* Leuchtende Körper werden nicht gezielt angepeilt (siehe 7.6: lieber `light`).
* Meshes kommen aus Listen (`mesh`, `lathe`, `heightfield`), noch nicht aus Modelldateien.
  Sie haben kein Inneres und können nicht in CSG.
* Glas mit einem Farb**muster** färbt das Durchgesehene, sein Schatten nimmt die flache Farbe.
