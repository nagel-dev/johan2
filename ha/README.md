# Home Assistant-aankondigingen op Johan2 AUX

De route is HA TTS -> Music Assistant -> Sendspin -> Johan2 AUX. Geen firmwarewijziging.
De geïnstalleerde HA-actie `music_assistant.play_announcement` accepteert bij de gebruiker
alleen een audio-URL, geen `message`. Daarom gebruikt de blueprint nu `tts.speak`.

## Handmatige test

Ga naar Ontwikkelaarstools -> Acties -> YAML:

```yaml
action: tts.speak
target:
  entity_id: tts.home_assistant_cloud
data:
  media_player_entity_id: media_player.macbook_pro_van_peter
  message: "De achterdeur is geopend."
  cache: true
```

De MacBook is de door de gebruiker geteste speler. Selecteer daarna de echte
HA-entiteit van Johan2 AUX om op de ESP32 te testen; de ID is niet live vastgesteld.
`tts.home_assistant_cloud` komt uit de bestaande HomeDash-configuratie: selecteer
een andere beschikbare HA TTS-entiteit als deze niet bestaat of niet beschikbaar is.
De stem en taal stel je in bij die HA TTS-engine. Gebruik Nederlands voor deze tekst.

Een OpenAI TTS-plugin die alleen in MA staat, verschijnt niet als HA `tts.*`-entiteit.
Deze eerste test gebruikt dus HA TTS, niet de OpenAI-stem van MA. Voor die stem via
een URL is later een aparte generatie-/API-route nodig, of een ondersteunde integratie.

Test eerst zonder muziek en daarna tijdens Spotify-weergave. Controleer of de
muziek weer doorgaat. Stel het aankondigingsvolume in bij de speler in MA; de
blueprint verandert geen hoofdvolume en doet geen eigen pause/resume.

## Blueprint installeren

Kopieer `blueprints/automation/johan2/door_announcement.yaml` naar
`/config/blueprints/automation/johan2/door_announcement.yaml` op de HA-host.
Open Instellingen -> Automatiseringen & scènes -> Blueprints en maak een
automatisering vanuit Johan2 - Deur geopend aankondiging. Selecteer deursensor,
Johan2 AUX en de HA TTS-engine. Bij een reeds geïnstalleerde versie: vervang het
bestand, herlaad de blueprint en selecteer de nieuwe TTS-engine-invoer.

Controleer dat de uit HomeDash gevonden `binary_sensor.achterdeur_sensor_2` bij
dicht `off` en bij open `on` aangeeft. Alleen een overgang van dicht naar open
die een seconde aanhoudt spreekt; onbekend/onbeschikbaar naar open niet.
Na het spreken geldt een cooldown van tien seconden. Er zijn geen stilletijden.

HA was vanaf de ontwikkelcomputer niet bereikbaar. Er is niets op de HA-host
geïnstalleerd; de YAML is gecontroleerd, uitvoering en muziekherstel nog niet.

Bronnen:
- https://www.home-assistant.io/integrations/tts/
- https://www.music-assistant.io/integration/announcements/

## Johan2 en HomeDash Assist koppelen

Installeer `johan2_assist.yaml` in `/config/packages/` en laad de HA-configuratie
nadat de configuratiecontrole slaagt. Het pakket bevat twee `input_button`-helpers,
`script.johan2_assist_start`, `script.johan2_assist_cancel`, de statushelper
`input_select.johan2_assist_state` en een watchdog voor achtergebleven status.

Schakel lokaal `JOHAN2_HOMEDASH_SYNC` en op HomeDash
`HOMEDASH_JOHAN2_ASSIST` in en bouw/flash beide apparaten. De bestaande HomeDash
Assistknop en `assist`-remotecommand gebruiken dan de microfoon van Johan2. Een
echte wake word en de knop gebruiken dezelfde Assist-pipeline en conversatie.
Start-/annuleerknoppen worden gericht als entiteiten gevolgd; het apparaataccount
heeft hiervoor geen toegang tot willekeurige HA-events nodig. Oude beginstanden
van de knoppen worden bij verbinden niet opnieuw uitgevoerd.

Controleer op hardware: (1) Hey Jarvis zet HomeDash op luisteren; (2) na het einde
van de spraak verdwijnt luisteren; (3) de remote start luisteren zonder wake word;
(4) annuleren keert terug naar idle en de volgende gesproken wake werkt weer.
Scripts/knoppen kunnen als doel van de bestaande remote-automatisering dienen.
