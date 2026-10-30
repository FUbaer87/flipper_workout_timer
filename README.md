# Zeitgeber für den Flipper Zero

Funktionen: **Stoppuhr** (mit Rundenzeiten), **Timer** (Countdown), **Intervall-Timer** (Vorlauf, Belastung/Pause, Runden – z. B. 60 s / 30 s) und eine **Signalwahl** (Vibration, Piepen oder Beides). Die LED zeigt zusätzlich die Phase an (blau = Vorlauf, grün = Belastung, rot = Pause, türkis = fertig).

## Bauen und installieren

1. uFBT installieren: `python3 -m pip install --upgrade ufbt`
2. Diesen Ordner öffnen und ausführen:
   - `ufbt launch` – baut, überträgt und startet die App (Flipper per USB verbinden)
   - oder `ufbt` – baut nur; die Datei `dist/interval_timer.fap` dann per qFlipper nach `SD Card/apps/Tools/` kopieren.
3. Bei einer Fehlermeldung zur SDK-Version: `ufbt update` bzw. `ufbt flash_usb`.

Die App liegt auf dem Flipper unter **Apps → Tools → Intervall-Timer**.

## Bedienung

**Menü:** Hoch/Runter wählen, OK öffnen. Bei „Signal“ mit Links/Rechts (oder OK) zwischen Vibration, Piepen und Beides wechseln – das gewählte Signal wird kurz vorgespielt. Zurück beendet die App.

**Stoppuhr:** OK = Start/Stopp, Rechts = Runde, Links = Reset (nur gestoppt), Zurück = Menü.

**Timer:** Links/Rechts ±5 s, Hoch/Runter ±1 min (gedrückt halten = größere Schritte), OK = Start.

**Intervall-Timer:** Hoch/Runter Feld wählen, Links/Rechts Wert ändern (gedrückt halten = größere Schritte), OK = Start. Nach der letzten Runde gibt es keine Pause.

**Während des Ablaufs:** OK = Pause/Weiter, Rechts = nächste Phase (nur Intervall), Links = aktuelle Phase neu starten, Zurück = Abbruch. In den letzten 3 Sekunden jeder Phase gibt es einen Countdown-Ton bzw. -Impuls.

Einstellungen (Zeiten, Runden, Signal) werden auf der SD-Karte gespeichert.
