# Fitzel — KI-Assistent (Ollama)

Fitzel kann ein **lokales Sprachmodell** fragen: über [Ollama](https://ollama.com), auf
diesem Rechner oder im Netz. Nichts geht ins Internet, es gibt kein Konto und keine
Kosten — nur das Modell, das man selbst installiert hat. Zwei Stellen nutzen es:

- die **Code-Editoren** (Lua: *Assets → Scripts*, C#: *Assets → C# Editor*) —
  Vorschläge beim Tippen und Änderungen per Anweisung,
- das Fenster **Assistant** (*View → Assets → Assistant (AI)*) — ein Chat, der den Editor
  selbst bedient: Objekte anlegen und ändern, Skripte schreiben und anhängen, Play.

Das Spiel selbst hat davon getrennt seine eigene Anbindung (die Lua-Tabelle `llm`,
siehe [lua-scripting.md](lua-scripting.md) §3.12.4); der Assistent gehört nur zum
Editor und wird nicht mit exportiert.

## 1. Einrichten

1. Ollama installieren und starten (`ollama serve`, unter Linux meist schon ein Dienst).
2. Ein Modell holen. Gut für beides — Code und Werkzeuge — ist ein Coder-Modell:

   | Modell | Grafikspeicher | Anmerkung |
   |---|---|---|
   | `qwen3-coder:30b` | ~20 GB | sehr gut, schnell (MoE) |
   | `qwen2.5-coder:14b` | ~10 GB | gut |
   | `qwen2.5-coder:7b` | ~6 GB | brauchbar, Vorschläge sehr flott |

   ```
   ollama pull qwen2.5-coder:7b
   ```
3. In Fitzel *View → Assets → Assistant (AI)* öffnen. Ohne Einstellung nimmt Fitzel das erste
   installierte Coder-Modell. *Settings* im Fenster (oder *AI → AI settings…* in einem
   Code-Editor) öffnet die Einstellungen:

| Einstellung | Bedeutung |
|---|---|
| Server | `http://127.0.0.1:11434` — ein anderer Rechner im LAN geht auch. Nur `http`. |
| Assistant | Das Modell für den Chat. Es muss **Werkzeuge** können (Tool Calling). |
| Code | Das Modell für Vorschläge und Strg+I („same as the chat“: dasselbe). |
| Context | Wie viel das Modell auf einmal liest (Tokens). Gilt für alle Anfragen gleich — Ollama lädt ein Modell neu, sobald sich die Größe ändert. |
| Suggest while typing | Vorschläge beim Tippen an/aus, Pause davor, höchstens so viele Zeilen. |
| Ask before changes | Jede Änderung an Szene oder Datei wartet auf *Allow*. |

Die Einstellungen liegen in `ollama.json` im Benutzerordner (neben `editor.json`).

**Grafikspeicher:** Das Modell belegt die Grafikkarte, solange Ollama es geladen hält
(Standard: 5 Minuten nach der letzten Frage). Ein 30B-Modell und eine große Szene
teilen sich dann den Speicher — bei Ruckeln ein kleineres Modell nehmen.

## 2. In den Code-Editoren

| Taste | Was passiert |
|---|---|
| (Pause beim Tippen) | Das Modell schlägt vor, was als Nächstes kommt — grau an der Schreibmarke, mehrzeilig als eingeschobene Zeilen. |
| **Tab** | Vorschlag übernehmen (ein Undo-Schritt). |
| **Strg+→** | Nur das nächste Wort des Vorschlags übernehmen. |
| **Esc** / weitertippen | Vorschlag verwerfen. Tippt man genau, was er sagt, läuft er mit. |
| **Alt+\\** (deutsche Tastatur: **Alt+#**) | Jetzt einen Vorschlag holen, ohne Pause. |
| **Strg+I** | Anweisung für die markierten Zeilen (ohne Markierung: neuer Code an der Schreibmarke). Die Änderung erscheint als Vorschau (rot/grün) unter der Stelle; **Strg+Enter** übernimmt sie als einen Undo-Schritt, eine weitere Anweisung verfeinert sie, **Esc** verwirft. |

Das Menü **AI** in beiden Editoren hat dazu:

- **Fix problem with AI** — das Problem nächst der Schreibmarke (C#: die Fehler des
  Sprachdienstes; Lua: der letzte Laufzeitfehler dieses Skripts) geht als Strg+I-Anweisung
  mit den Zeilen darum ans Modell.
- **Explain in Assistant** — die Markierung (oder der Code um die Schreibmarke) als
  Frage ins Assistant-Fenster.

Das Modell kennt die Engine nicht von selbst. Fitzel gibt ihm die API mit: für Lua die
Liste, aus der auch die Completion kommt (`luacomplete::apiSummary`), für C# die Kapitel
3–4 aus [csharp-scripting.md](csharp-scripting.md).

Vorschläge kommen über „Fill in the Middle“ (Text vor *und* nach der Schreibmarke).
Das können Coder-Modelle — Qwen-Coder, CodeLlama, DeepSeek-Coder, StarCoder, CodeGemma
und alle, deren Ollama-Vorlage einen Suffix kennt. Andere Modelle werden in Worten
gefragt; das geht auch, ist aber langsamer und ungenauer.

## 3. Das Assistant-Fenster

Ein Chat — mit dem Unterschied, dass der Assistent **handelt**, statt zu erklären,
wo man klicken müsste. Beispiele:

- „Was ist in dieser Szene?“
- „Stell einen roten 2-m-Würfel vor mich und lass ihn langsam drehen.“
- „Schreib ein Skript, mit dem ich das ausgewählte Objekt mit WASD bewege.“
- „Bau eine Treppe aus acht Kisten.“

Seine Werkzeuge:

| Werkzeug | Tut |
|---|---|
| `list_objects`, `get_object`, `get_selection` | Szene ansehen: Objekte, Transform, Komponenten mit Eigenschaften |
| `create_object`, `modify_object`, `delete_object`, `duplicate_object` | Box, Kugel, Zylinder, Rampe, Ebene, Licht, Empty; Name, Position, Drehung, Größe, aktiv, Eltern |
| `set_color` | Farbe (ein Material dieser Farbe, geteilt), Rauheit, Metall, Leuchten; bei Lichtern die Lichtfarbe |
| `list_component_types`, `add_component`, `set_component`, `remove_component` | alle Komponenten, die das Inspector-Menü anbietet |
| `list_scripts`, `read_script`, `write_script`, `attach_script` | Skripte unter `scripts/` lesen, schreiben, an Objekte hängen (mit Parametern) |
| `play`, `stop`, `read_log`, `script_errors` | ausprobieren und nachsehen, was schiefging |
| `select`, `undo`, `redo`, `save_scene` | Auswahl (die Ansicht fährt hin), Rückgängig, Speichern |
| `list_assets`, `place_asset` | Modelle (`.glb`) und Prefabs des Projekts und der Engine finden und auf den Boden stellen |
| `create_road`, `list_roads` | eine Straße durch Kontrollpunkte legen, mit Höhen und Brücken-Abschnitten, und ins Gelände bauen (wie *Build* im Roads-Panel) |
| `set_weather`, `list_weather` | Wetter-Preset und Tageszeit im Editor setzen |
| `open_panel` | ein Panel aus dem View-Menü öffnen — für alles, was kein Werkzeug kann (Gelände, Städte, Vegetation …) |
| `search_docs`, `scripting_reference` | in den Handbüchern nachschlagen (README, Feature-Liste, Editor-Bedienung, Lua, C#, diese Seite) |
| `describe_component` | was eine Komponente tut und was ihre Eigenschaften bedeuten — aus den Kommentaren ihres Quelltexts |
| `read_example` | die mitgelieferten Beispielskripte lesen (`walker.lua`, `door.lua`, `inventory.lua` …) |

Was der Assistent über Fitzel weiß, kommt aus drei Quellen, nicht aus seinem Training:

- **Der Systemprompt** enthält eine Kurzfassung von [FEATURES.md](FEATURES.md) (ein Satz pro
  Funktion), die Panels des View-Menüs, die Bedienung auf einen Blick (Kapitel 1 von
  [editor.md](editor.md): Kamera, Auswahl, Gizmo, Tasten), die Skript-API und die Liste der
  Komponenten.
- **Jede Frage** bekommt unsichtbar die passendsten Handbuch-Kapitel mit, auch aus
  [editor.md](editor.md), dem Handbuch zur Bedienung (deutsche Begriffe
  finden auch die englische README: Straße → road, Stadt → town …), und, wenn eine Komponente
  beim Namen genannt wird, deren Definition samt Kommentaren.
- **Nachschlagen** per `search_docs`, `describe_component` und `read_example`. Die Handbücher
  und die Komponenten-Header legt der Build neben die exe (`docs/`, `docs/components/`).

Dazu ein paar Leitplanken: Schreibt er ein Lua-Skript, prüft `write_script` es sofort
(Syntax, Felder, die `e` nicht hat, Zustand auf `e`, `game.`-Funktionen, die es nicht gibt)
und meldet Fehler zurück, damit er es neu schreibt. Scheitern drei Werkzeug-Aufrufe in
Folge, wird er angewiesen aufzuhören und zu sagen, was nicht ging — statt die Szene
„umzubauen“. Fragen („wie …?“) beantwortet er, ändern tut er nur auf Auftrag.

Jede Szenen-Änderung ist ein normaler Undo-Schritt — **Strg+Z** nimmt zurück, was das
Modell getan hat. Dateien (Skripte) sind davon ausgenommen; wer das nicht will, schaltet
*Ask before changes* ein: dann wartet jede ändernde Aktion auf *Allow*, *Allow all this
time* (für den Rest der Antwort) oder *Refuse*.

Unter jeder Antwort steht, was der Assistent aufgerufen hat — aufklappen zeigt
Argumente und Ergebnis. **Stop** bricht eine Antwort ab, **New chat** fängt neu an.
Ein Gespräch, das länger wird als der Kontext, verliert seinen Anfang.

## 4. Prüfen

`assistcheck` (in `build/<baum>/bin`) fährt das alles gegen ein laufendes Ollama:
Vorschläge in Lua und C# samt Tab und Undo, Strg+I mit Vorschau und Übernehmen, und
im Assistant-Fenster eine rote Kiste mit Spin-Komponente und ein angehängtes
Wipp-Skript. Ohne Server endet es mit Code 3.

```
cd build/linux/bin && ./assistcheck /tmp/assist
```

Was das Modell weiß, prüft man mit Fragen statt mit Checks: `probe <datei>` stellt jede
Zeile der Datei in einem frischen Gespräch und druckt, welche Handbuch-Kapitel mitgingen,
welche Werkzeuge es aufrief und was es antwortete (Panels, Wetter und Straßen sind dort
Attrappen, die mitschreiben).

```
cd build/linux/bin && ./assistcheck /tmp/assist http://127.0.0.1:11434 probe fragen.txt
```
