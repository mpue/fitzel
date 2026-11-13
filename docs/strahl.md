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
(`sandbox/src/Strahl*.cpp`, `TraceShapes.cpp`, `TraceFunc.cpp`).

---

## Starten

**Im Editor:** *View > Presentation > Strahl (scene script)*. Links das Skript, rechts das
Bild. Einen Moment nach dem Tippen läuft das Skript erneut (in einem eigenen Thread) und ein
Entwurf (640 px, wenige Samples) baut sich auf — *Follow typing* schaltet das ab.

| Taste / Knopf | Wirkung |
| ------------- | ------- |
| **Preview** / F5 | Entwurf jetzt rechnen |
| **Render** / Strg+Enter | volle Qualität mit den Einstellungen aus `render { }` |
| **Save** / Strg+S | Skript speichern (neue Skripte und Beispiele landen in `<Projekt>/strahl/`) |
| **Save image** | PNG + lineares `.exr` nach `<Projekt>/renders/` |
| **Open...** | Beispiele, die Skripte des Projekts, oder eine beliebige Datei |

**Die Kamera im Bild bewegen.** Im Vorschaubild steuert die Maus die Kamera; ein kleines
schnelles Bild folgt ihr, und beim Loslassen (oder wenn das Rad stillsteht) werden
`position`, `look_at` (bzw. `direction`) und, wenn geändert, `fov` in den `camera { }`
des Skripts geschrieben — als ein Schritt, den Strg+Z zurücknimmt — und das Skript läuft
neu. Fehlt ein Wert, wird er ergänzt; hat das Skript keine Kamera, kommt eine hinter
`render { }`. Ein berechneter Wert (`[5 * sin(time * 360), 2, 5]`, eine Variable) wird
nicht überschrieben: Das Bild zeigt die bewegte Kamera, unter ihm steht, warum das Skript
unverändert blieb.

| Maus im Bild | |
| ------------ | --- |
| links ziehen | um den `look_at`-Punkt drehen |
| rechts ziehen | umschauen (der Punkt wandert mit) |
| Mitte ziehen / Umschalt+links | verschieben |
| Rad | näher heran / weiter weg |
| Strg+Rad | Bildwinkel (`fov`) |

Ein Fehler unterstreicht die Stelle im Skript und steht rot unter dem Bild (anklicken
springt hin); das Bild bleibt das letzte, das gebaut hat. `print()` erscheint unter *Output*.
`sandbox --strahl datei.strahl` öffnet den Editor gleich mit einem Skript.

**Vervollständigung.** Beim Tippen eines Namens klappt eine Liste auf, mit dem, was an
dieser Stelle passt, zuerst:

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

**Auf der Kommandozeile:**

```bash
build/linux/bin/strahl szene.strahl
```

| Option | |
| ------ | --- |
| `-o bild.png` | Ziel (sonst `render { output }` oder `szene.png` neben dem Skript) |
| `-s N`, `-w N`, `-h N`, `-b N` | Samples, Breite, Höhe, Bounces überschreiben |
| `-D name=wert` | ein `let` auf oberster Ebene vorbelegen (ein Strahl-Ausdruck: `-D farbe=#f00`) |
| `--frames N` | Animation: N Bilder, `time` läuft 0..1, nummerierte PNGs |
| `--exr` | zusätzlich das lineare Bild |
| `--show base\|normal\|depth` | Diagnose statt Render |
| `--check` | nur bauen und berichten |
| `-L ordner` | weiterer Bibliotheksordner für `import` |

Die Beispiele liegen in `sandbox/strahl/examples/` (beim Build nach `bin/strahl-lib/examples/`
kopiert): `basics`, `csg`, `materials`, `isosurfaces`, `glass`, `city`, `turntable`.

---

## Grundlagen

**Einheiten:** Meter. **Achsen:** rechtshändig, y nach oben (wie die Engine). **Winkel:
immer Grad** — in `rotate`, `fov`, und auch in `sin`, `cos`, `atan2` & Co.
(`sin(90) == 1`). `radians()` und `degrees()` rechnen um.

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

Operatoren, schwach bindend zuerst: `or ||` · `and &&` · `not` · `== != < <= > >=` ·
`.. ..=` · `+ - ++` · `* / %` · unäres `- !` · `^` (Potenz, rechtsassoziativ). `++` hängt
Listen und Texte aneinander, `+` mit einem Text ebenfalls. `%` ist immer positiv.

Kommentare: `// bis Zeilenende` und `/* ... */`. Anweisungen brauchen kein Trennzeichen;
`;` und `,` sind überall erlaubt, wo eine Anweisung endet. Eine `(` oder `[` am
**Zeilenanfang** beginnt einen neuen Ausdruck, statt den der vorigen Zeile aufzurufen.

### Namen, Funktionen, Ablauf

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

`if` ist ein Ausdruck: `let t = if a > 1 { "groß" } else { "klein" }`. Schleifen:
`for x in liste/bereich/vektor`, `for i, x in liste` (mit Index), `while bedingung { }`,
dazu `break`, `continue`, `return`. Ein Block liefert den Wert seiner letzten Zeile — so
geben Funktionen ohne `return` ihr Ergebnis zurück. Rekursion geht (bis Tiefe 200).

`import "datei"` (`.strahl` darf fehlen) führt eine Datei einmal aus; ihre `let` und `fn`
stehen danach zur Verfügung. Gesucht wird neben dem Skript, dann in der Bibliothek
(`strahl-lib/`, z. B. `import "studio"` für Hohlkehle und Studiolicht).

---

## Objekte

Ein Objekt ist eine Art und ein Block von Eigenschaften:

```strahl
sphere { center: [0, 1, 0], radius: 0.5, material: gold }
sphere([0, 1, 0], 0.5, material: gold)       // dasselbe als Aufruf
sphere { [0, 1, 0], 0.5, gold }               // unbenannt, in der Reihenfolge der Art
```

Unbenannte Werte füllen die Parameter der Art der Reihe nach (bei `sphere`: `center`,
`radius`); ein Material darunter ist das Material, wo immer es steht. Benannte Werte
gewinnen gegen unbenannte, spätere gegen frühere.

**Ändern heißt kopieren.** Ein Block hinter einem vorhandenen Objekt macht eine geänderte
Kopie; das Original bleibt:

```strahl
let saeule = cylinder { from: [0, 0, 0], to: [0, 3, 0], radius: 0.2 }
saeule { translate: [-2, 0, 0], material: marble() }
saeule { translate: [ 2, 0, 0], material: granite() }
```

Das gilt auch für das Ergebnis einer Funktion: `ring(8) { material: chrome, translate: [0, 1, 0] }`.

**Transformationen** wirken in der Reihenfolge, in der sie dastehen, und jede nach den
vorigen: `translate: [x, y, z]`, `rotate: [x°, y°, z°]` (erst um x, dann y, dann z),
`scale: 2` oder `scale: [sx, sy, sz]` (auch ungleichmäßig), `matrix: m` mit einer Matrix
aus `translation()`, `rotation()`, `rotation_axis()`, `scaling()` (multiplizierbar, `m * p`
transformiert einen Punkt).

### Wohin Objekte gehen

* Eine Anweisung, deren Wert ein Objekt ist, legt es in die Szene.
* In den `{ }` eines Objekts werden Objekte zu seinen **Kindern** (CSG-Operanden,
  Gruppenmitglieder), andere Werte zu seinen unbenannten Parametern.
* In einer Funktion werden erzeugte Objekte gesammelt und **zurückgegeben** — eins als es
  selbst, mehrere als `group`. Darum verhält sich eine Funktion wie ein Konstruktor.
* Was nur berechnet wird — rechts von `let`, als Argument, ein `if` als Wert — legt nichts
  ab. `let k = sphere { }` erzeugt die Kugel, zeigt sie aber nicht.

---

## Körper

Alle Körper nehmen `material`, `translate`, `rotate`, `scale`, `matrix`.

| Art | Eigenschaften (unbenannt in dieser Reihenfolge zuerst) | |
| --- | ------------------------------------------------------ | --- |
| `sphere` | **center**, **radius** | |
| `box` | **min**, **max** — oder `size` (Zahl oder Vektor) und `center` | Vorgabe: Würfel 1 m um den Ursprung |
| `plane` | **normal**, **offset** — oder `point` | unendlich; fest ist die Seite, von der die Normale wegzeigt |
| `disc` | **center**, **normal**, **radius**, `hole` | ohne Volumen (nicht in CSG sinnvoll) |
| `cylinder` | **from**, **to**, **radius**, `open` | `open: true`: ohne Deckel |
| `cone` | **from**, **to**, **radius**, **top_radius**, `open` | Kegelstumpf, `top_radius` 0 = Spitze |
| `torus` | **major**, **minor** | liegt in der x-z-Ebene |
| `isosurface` | **function**, `bound` / `min`, `max`, `threshold`, `max_gradient`, `accuracy`, `open` | siehe unten |
| `mesh` | **vertices**, **faces**, `normals`, `uvs`, `smooth` | Dreiecke/Polygone; nicht in CSG |

Aus der Bibliothek (in Strahl geschrieben, `sandbox/src/StrahlPrelude.cpp`):

| Funktion | |
| -------- | --- |
| `rounded_box(size, radius, center)` | Würfel mit gerundeten Kanten (exakte Distanzfunktion) |
| `capsule(from, to, radius)` | Zylinder mit Halbkugeln |
| `blob(centers, radius, threshold)` | Metaballs |
| `lathe(points, segments)` | Rotationskörper aus einem Profil `[[r, y], ...]` von unten nach oben |
| `heightfield(f, size, resolution)` | Fläche `y = f(x, z)` |
| `cyclorama()`, `studio_lights()` | Hohlkehle und Drei-Punkt-Licht (`import "studio"`) |

### CSG

| | Ergebnis |
| --- | --- |
| `union { a b ... }` | alles zusammen (auf oberster Ebene einfach eine Gruppe) |
| `merge { ... }` | wie union, aber ohne innere Flächen — für Glas |
| `intersection { ... }` | nur was in allen liegt |
| `difference { a b c }` | `a` ohne `b` und ohne `c` |
| `group { ... }` / `object { ... }` | nur gemeinsam bewegen und färben |

Operanden ohne eigenes Material erben das der Kombination. CSG ist exakt: der Strahl läuft
die Kreuzungen der Operanden entlang und behält die, die die Regel erfüllen. Ebenen sind
gute Operanden (`intersection` aus Ebenen = Polyeder, siehe den Diamanten in
`examples/glass.strahl`). Flächen, die genau aufeinanderliegen, sind wie in jedem
CSG-Raytracer zweideutig — einen Hauch überstehen lassen.

---

## Materialien

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
    medium_color: #...      // Eigenfarbe des Glaskörpers (Wasser)
    translucency: 0         // Anteil Licht, der auf der Rückseite austritt (Blatt, Papier)
    opacity: 1              // < 1: teildurchsichtig
    bump: fn(p) => ...      // Höhenfeld in Metern, kippt die Normale
    bump_strength: 1
    shadow: true            // false: wirft keinen Schatten
    translate / rotate / scale   // verschieben das Muster gegenüber dem Körper
}
```

Statt eines Materials geht auch eine Farbe (`material: #f00`, ein Kunststoff) oder ein
Muster (`material: marble()`).

Ein Material ohne Namen in den `{ }` eines Materials ist sein **Ausgangspunkt**, wie
`texture { T_Stone … }` in POV-Ray: `material { gold, roughness: 0.3 }` ist Gold mit
anderer Rauheit, `material { matte(checker(#ddd, #555, size: 0.5)) }` das matte Schachbrett.
Was im Block steht, gewinnt; eine Farbe ohne Namen dahinter ersetzt die Farbe. Dasselbe
geht als Kopie: `gold { roughness: 0.3 }`.

Ein Material ohne Namen in den `{ }` eines Materials ist sein **Ausgangspunkt**, wie
`texture { T_Stone … }` in POV-Ray: `material { gold, roughness: 0.3 }` ist Gold mit
anderer Rauheit, `material { matte(checker(#ddd, #555, size: 0.5)) }` das matte Schachbrett.
Was im Block steht, gewinnt; eine Farbe ohne Namen dahinter ersetzt die Farbe. Dasselbe
geht als Kopie: `gold { roughness: 0.3 }`. Vorlagen: `matte(c)`, `plastic(c, roughness)`,
`metal(c, roughness)`, `glass(c, ior, absorption)`, `emissive(c, strength)`; fertig:
`chrome mirror silver gold copper brass steel aluminium water crystal diamond`.

Farbiges Glas färbt man am besten über `absorption` (pro Meter verlorenes Licht je Kanal):
dünnes Glas bleibt hell, dickes wird satt. `color` tönt dagegen jeden Durchgang gleich.

**Muster leben im Raum des Objekts**, dem das Material gegeben wurde, vor dessen eigenen
Transformationen — ein Marmorblock behält seine Adern, wenn er bewegt wird, und ein
`difference` zeigt im Schnitt den Marmor von innen.

---

## Funktionen pro Strahl: Muster, Bump-Maps, Isoflächen

Ein Muster ist eine Funktion des Punktes: `fn(p) => ...`, optional `fn(p, n, uv)` mit der
Weltnormalen `n` und den Flächenkoordinaten `uv`. Zurück kommt eine Farbe (oder eine Zahl
für roughness, metallic, bump, isosurface).

```strahl
material { color: fn(p) => mix(#203060, #e0e8ff, smoothstep(-0.3, 0.3, noise(p * 4))) }
```

Solche Funktionen werden **beim Bauen in Bytecode übersetzt** (`TraceFunc.hpp`) und laufen
pro Strahl ohne den Interpreter. Dabei wird alles, was nicht vom Punkt abhängt, sofort
ausgerechnet — Aufrufe, die Funktionen liefern, Farblisten, Bilder. Übrig bleibt die
Arithmetik auf dem Punkt. Pro Strahl erlaubt: Zahlen und Vektoren bis 3 Komponenten,
`let`, Zuweisung, `if`, `return`, Aufrufe (werden eingesetzt), `for` über **vorher bekannte**
Bereiche/Listen (wird ausgerollt, höchstens 256 Durchläufe). Nicht erlaubt (mit Meldung):
`while`, Rekursion, Listen oder Objekte bauen, Funktionen definieren.

**Bausteine:** alle Mathefunktionen, `length normalize dot cross distance reflect`,
`noise(p)` (Perlin, ca. −1..1), `fbm(p, oktaven)`, `turbulence(p, oktaven)` (0..1),
`voronoi(p)` (Abstand zum nächsten Zellpunkt), `cell_id(p)` (0..1 je Zelle), `hash(x)`,
`parity(p)` (0/1-Schachbrett), `random(seed)`, `ramp(t, [farbe, [0.4, farbe], ...])`,
`texture(bild, uv)` mit `load_image("datei.png")`, `at(muster, p)` (Farbe oder Muster
an einem Punkt — damit lassen sich Muster schachteln).

**Fertige Muster:** `checker(a, b, size)`, `stripes(a, b, width, axis)`, `rings`, `gradient`,
`clouds`, `marble`, `wood`, `granite`, `spots`, `bricks`, `tiles`, `image(datei, scale)`,
`planar(datei, size)`. Ihre Farbargumente dürfen selbst Muster sein:
`checker(marble(), wood(), size: 0.25)`.

**Bump-Maps** (Höhe in Metern): `bumps`, `wrinkles`, `dents`, `ripples`, `waves`, oder eine
eigene Funktion. Die Normale wird um den Gradienten gekippt; die Geometrie bleibt.

### Isoflächen

```strahl
isosurface {
    function: fn(p) => length(p) - 0.7 + 0.15 * noise(p * 4)   // < 0 innen
    bound: 1              // Würfel ±1 (oder [x, y, z], oder min/max)
    threshold: 0
    max_gradient: 2       // wenn weggelassen: gemessen (steht im Log)
}
```

Die Fläche wird abgetastet: Schritte so groß, wie `|f| / max_gradient` erlaubt, an einem
Vorzeichenwechsel per Bisektion verfeinert. Zu kleines `max_gradient` lässt dünne Stellen
aus (Löcher), zu großes kostet Zeit. Ohne Angabe misst Strahl den Gradienten nahe der
Fläche und schreibt das Ergebnis ins Log. Wo die Box den Körper schneidet, schließt sie ihn
(außer `open: true`). Isoflächen sind volle CSG-Operanden.

---

## Licht, Himmel, Kamera, Render

| Art | Eigenschaften | |
| --- | ------------- | --- |
| `sun` | **direction** (zur Sonne hin), **color**, `intensity` (3), `angle` (Scheiben-Durchmesser in Grad, 0.53) | höchstens eine; größeres `angle` = weichere Schatten |
| `light` | **position**, **color**, `intensity` (10), `radius` (0.05), `range`, `visible` | Punktlicht; `intensity` ist die Beleuchtung in 1 m Abstand, fällt mit 1/d² |
| `spot` | **position**, **look_at** (oder `direction`), `color`, `intensity` (20), `angle` (Kegel-Halbwinkel, 30), `inner`, `radius`, `range`, `visible` | |
| `sky` | **zenith**, **horizon**, **ground**, `intensity`, `hdri: "datei.hdr"`, `rotate`, `procedural: true`, `background` | Verlauf, Panorama (.hdr/.exr) oder der Himmel der Engine |
| `fog` | **density**, **color**, `height`, `falloff`, `sun_color` | Höhennebel wie im Viewport |
| `camera` | **position**, **look_at** (oder `direction`), `up`, `fov` (vertikal, 45), `aperture` (Blendenradius in m), `focus` (Meter oder `"auto"`), `roll` | |
| `render` | `width`, `height`, `samples`, `bounces`, `seed`, `threads`, `clamp`, `exposure`, `tonemap` (`"aces"`, `"agx"`, `"neutral"`), `saturation`, `contrast`, `warmth`, `hue`, `vignette`, `output` | |

`light { visible: true }` macht die Glühbirne für die Kamera sichtbar (sie selbst wirft
kein zusätzliches Licht). Leuchtende Materialien beleuchten die Szene nur über Abpraller — für eine
Lampe ist `light` mit `radius` die rauschärmere Wahl. `sky { background: farbe }` ist, was
die Kamera hinter allem sieht; Licht kommt weiter vom Himmel (nicht für Glasszenen: auch
Brechungsstrahlen sehen den Hintergrund). Kamera, Lichter und Sonne lassen sich wie Körper
transformieren und in Gruppen bewegen.

### Animation

`time` läuft über `--frames N` von 0 bis 1, `frame` zählt, `frames` ist N:

```strahl
camera { position: [5 * sin(time * 360), 2, 5 * cos(time * 360)], look_at: [0, 0.7, 0] }
```

```bash
build/linux/bin/strahl turntable.strahl --frames 120
```

---

## Eingebaute Funktionen

| Gruppe | |
| ------ | --- |
| Mathe | `sin cos tan asin acos atan atan2` (Grad), `sqrt pow exp log log2 log10 abs sign floor ceil round fract mod min max clamp mix lerp smoothstep step saturate radians degrees` — komponentenweise auf Vektoren |
| Vektoren | `vec(...)`, `length normalize dot cross distance reflect` |
| Farbe | `rgb hsv gray kelvin ramp at` |
| Rauschen | `noise fbm turbulence voronoi cell_id hash parity random` |
| Listen | `len range list push pop map filter reverse sum contains`, `min(liste)`, `max(liste)` |
| Matrizen | `identity translation rotation rotation_axis scaling inverse apply apply_dir` |
| Sonst | `str print error assert type load_image texture` |
| Konstanten | `pi tau e inf`, die Achsen `x y z`, Farben `white black red green blue yellow orange cyan magenta purple brown pink ivory` |

---

## Grenzen

* **Nur der CPU-Tracer** kennt exakte Körper und Musterfunktionen. Der GPU-Tracer (Viewport-
  Vorschau, `--shots-trace-gpu`) bekommt Strahl-Szenen nicht zu sehen.
* Keine Kaustiken: Licht durch Glas wird nicht gebündelt; der Schatten eines Glaskörpers ist
  ein getönter Halbschatten.
* Leuchtende Körper werden nicht gezielt angepeilt (siehe oben: lieber `light`).
* Meshes kommen aus Listen (`mesh`, `lathe`, `heightfield`), noch nicht aus Modelldateien.
  Sie haben kein Inneres und können nicht in CSG.
* Glas mit einem Farb**muster** färbt das Durchgesehene, sein Schatten nimmt die flache Farbe.
