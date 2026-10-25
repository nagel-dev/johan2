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

## Douchetimer

Installeer `shower_timer.yaml` als `/config/packages/shower_timer.yaml`, naast
HomeDash's bestaande `announce.yaml` en `voice_failover.yaml`. Het pakket gebruikt
`timer.douchetimer`, `script.announce` en `input_select.voice_device`; de firmware
hoeft niet te worden aangepast. Getest met HA 2026.9.3; de herstelafhandeling
gebruikt het timerattribuut `last_transition` uit HA 2026.9 en nieuwer.
Voer de HA-configuratiecontrole uit en herstart HA om de nieuwe helper te laden.

De bestaande UI-helper `timer.douchetimer` heeft al tien minuten als standaard
en **Herstellen** ingeschakeld. Laat deze bestaan en definieer hem niet nogmaals
in YAML. Alleen bij een nieuwe installatie zonder deze helper kan dit onderaan
het pakket worden toegevoegd:

```yaml
timer:
  douchetimer:
    name: Douchetimer
    icon: mdi:shower-head
    duration: "00:10:00"
    restore: true
```

Zeg na Hey Jarvis bijvoorbeeld:

- "Start de douchetimer" of "Ik ga douchen": altijd tien minuten, ook na een
  eerdere timer met een andere duur.
- "Zet de douchetimer op 8 minuten" of "Stel de douche timer in op acht minuten":
  een heel aantal van 1 tot en met 60 minuten, als cijfers of Nederlandse woorden.
- "Stop de douchetimer": annuleren zonder eindmelding.
- "Hoeveel tijd heeft de douchetimer nog?" of "Hoe lang heb ik nog om te douchen?":
  resterende tijd, gepauzeerd of geen actieve timer.

De zinnen zijn conversation-triggers en geven een eigen Assist-antwoord. HA
2026.9.3 behandelt een passende zin vóór de geselecteerde OpenAI-agent; hiervoor
hoeft `prefer_local_intents` niet aan. Vrije formuleringen en sommige
vervolggesprekken kunnen wel bij de AI-agent uitkomen. Maak daarom via
Instellingen -> Spraakassistenten -> Blootstellen ook expliciet deze twee
scripts beschikbaar voor **Assist**:

- `script.shower_timer_start` (Douchetimer - starten)
- `script.shower_timer_cancel` (Douchetimer - stoppen)

Laat `timer.douchetimer` ook zichtbaar voor Assist. Schakel niet automatisch alle
nieuwe entiteiten in. De OpenAI-conversatieagent moet de bestaande **Assist**-API
gebruiken. De scriptvelden geven de AI de optionele parameter `minutes` (1-60,
standaard 10). Het beschreven intent `Johan2ShowerTimerStatus` levert automatisch
een alleen-lezen AI-tool voor de echte resterende tijd. De omschrijvingen maken
duidelijk dat deze HA-helper werkt wanneer native timers op het spraakapparaat
niet worden ondersteund. Alleen de timer zichtbaar maken is onvoldoende om de
startactie aan de AI beschikbaar te stellen.

Een dashboard/automatisering kan dezelfde `script.shower_timer_start` aanroepen
met optioneel `minutes: 8`, of `script.shower_timer_cancel`. Ongeldige waarden
laten het startscript met een fout stoppen, zodat een AI-toolaanroep geen
geslaagde start kan melden na een stil overgeslagen validatie.

Tijdens het aftellen volgt één bericht bij vijf minuten resterend en één bij
één minuut. Daarna: "Je douchetijd is voorbij." De berichten gebruiken
`script.announce` met `target: owner`: Johan2 wanneer die de actieve stem bezit,
anders HomeDash volgens de bestaande failover. Ze negeren stilletijden omdat de
timer expliciet is gestart. Het pakket verandert het volume niet.

Een start met hoogstens vijf minuten slaat het vijfminutenbericht over; een start
met hoogstens één minuut slaat beide herinneringen over. Starten, hervatten of
een wijziging via `timer.change` berekent de drempels opnieuw vanaf de actuele
eindtijd. Pauzeren via HA onderbreekt herinneringen. Annuleren wist de planning.
De kleine `input_text.shower_timer_checkpoint` bewaart planning en verzonden
drempels bij herstart; verander deze helper niet handmatig tijdens gebruik.
Als de timer tijdens HA-uitval afloopt, volgt de eindmelding na het opstarten.
Er wordt geen oude eindmelding afgespeeld bij de eerste installatie zonder
bijbehorend checkpoint. Een lange vertraging geeft alleen de nog relevante
herinnering en geen reeks achterstallige berichten.

### Controleren

Lokale template-/grammaticatests (vereist PyYAML, Jinja2 en hassil):

```bash
python3 tests/shower_timer_test.py
```

Controleer op HA: starten zonder/met duur en de bevestiging, status, annuleren,
en een start zonder duur na een afwijkende duur. Test met `timer.start` en
`duration: 65` het éénminutenbericht en einde; gebruik `duration: 305` voor het
vijfminutenbericht. Een start van 30 seconden geeft alleen een eindmelding.
Controleer ook pauze/hervatten, een gewijzigde eindtijd, herladen/herstarten
tijdens aftellen en afloop tijdens HA-uitval.

De lokale tests vervangen de HA-configuratiecontrole en fysieke luistertest
niet. Controleer afzonderlijk verstaanbare spraak op Johan2, hervatten van muziek,
en HomeDash als actieve eigenaar na failover.

Op 2026-10-25 is het pakket op de bestaande HA 2026.9.3 geïnstalleerd en de
volledige configuratiecontrole geslaagd. Tekstinvoer door de geselecteerde
OpenAI Assist-pipeline bevestigde de lokale start-/duur-/status-/stopantwoorden
en de terugkeer naar tien minuten na een afwijkende duur. Versnelde timers
bereikten beide herinneringen en de eindmelding via `script.announce`, met
Johan2 als eigenaar. Annuleren gaf geen eindmelding en einde werd niet herhaald.
Een timer die tijdens een HA-herstart afliep gaf één eindmelding na opstarten.
Ook de eerste start met een nog onbekend checkpoint is live gecontroleerd.
De timer is gestopt achtergelaten; fysiek horen en HomeDash-failover zijn nog
niet afzonderlijk getest.

Na een gemelde gesproken weigering is op dezelfde dag ook de echte AI-route
hersteld en getest. De start- en stopscripts waren nog niet aan Assist
blootgesteld. Beide zijn nu ingeschakeld; de AI gebruikt aantoonbaar de
start-/stoptools en het statusintent, ook binnen één gesprek en bij vrije
formuleringen. Rechtstreeks "start de douchetimer" aan de AI geven startte
daarna een echte timer van tien minuten. Die testtimer is weer gestopt. Dit
controleert de AI-acties; de herkende tekst van de eerdere mislukte
microfoonopdracht was niet beschikbaar en fysiek opnieuw inspreken blijft
een afzonderlijke controle.

Bronnen:
- https://www.home-assistant.io/integrations/timer/
- https://www.home-assistant.io/voice_control/custom_sentences/
