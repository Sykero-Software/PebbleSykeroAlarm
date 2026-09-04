# Miten Sykerö Smart Alarm toimii

*In English: [how-it-works.md](how-it-works.md)*

Tämä kertoo, mitä sovellus oikeasti tekee — riittävän tarkasti, että voit ennustaa
sen käyttäytymisen ja olla siitä eri mieltä lukematta koodia. Teksti on kirjoitettu
herätyskellon käyttäjälle, ei sen ylläpitäjälle.

**Lupaus:** se soi. Kaikki fiksu tässä sovelluksessa on tavallisen herätyskellon
*edessä*: herätys soi asettamaanasi aikana, eikä mikään tässä voi siirtää sitä
myöhemmäksi. Jos liikedataa ei ole, kello ei ollut ranteessa, tai tunnistin ei
yksinkertaisesti löydä hyvää hetkeä, sinut herätetään silti herätysaikana.

Poikkeuksia on tasan yksi, ja se kannattaa tietää ennen kuin luotat tähän
sovellukseen: **jos akku laskee kellon omaan virransäästötilaan, mikään sovelluksen
herätys ei voi soida** — kello sammuttaa silloin palvelun, josta jokainen sovelluksen
herätys riippuu. Katso luku 5. Pidä kello ladattuna, tai aseta kellon sisäänrakennettu
herätys varmistukseksi yönä jolla on väliä.

---

## Lyhyesti

Asetat herätykset puhelimella. Kello säilyttää ne, ja vähän ennen herätystä se alkaa
seurata minuutti minuutilta kuinka paljon liikut. Kun se näkee sinun liikahtavan —
mitattuna sitä vasten, kuinka liikkumatta *sinä* olet ollut tänä yönä, ei kiinteää
lukua vasten — se soittaa silloin. Ajatuksena on, että kevyestä unesta herääminen on
helpompaa kuin syvästä unesta raahaaminen muutamaa minuuttia myöhemmin. Jos hyvää
hetkeä ei koskaan tule, herätysaika itse soittaa.

---

## 1. Mitä herätysaika tarkoittaa

Tämä kannattaa saada ensin oikein, koska kaikki kolme vastausta ovat järkeviä eikä
sovellus voi arvata kumpaa tarkoitit.

![Herätysajan kolme merkitystä](img/semantics.fi.svg)

- **The latest — may ring earlier** (*viimeisin hetki*, oletus). Herätys 07:50
  tarkoittaa "ole hereillä 07:50 mennessä". Kello tarkkailee 07:20–07:50 ja soittaa
  ensimmäisellä hyvällä hetkellä.
- **The earliest — may ring later** (*aikaisin hetki*). 07:50 tarkoittaa "älä herätä
  minua ennen 07:50". Kello tarkkailee 07:50–08:20 ja soittaa viimeistään 08:20.
- **When I must be fully awake** (*silloin täysin hereillä*). 07:50 on hetki, jolloin
  herätyksen pitää olla *täydessä voimassaan*, joten voimistuminen alkaa niin
  aikaisin että se on siihen mennessä kehittynyt.

Älyherätyksen sammuttaminen saa kaikki kolme soimaan tasan asettamanasi aikana.

Ikkunan pituus (10–60 minuuttia, oletus 30) on se mitä "vähän ennen" tarkoittaa, ja
se on sovelluksen keskeisin vaihtokauppa: pidempi ikkuna antaa tunnistimelle enemmän
tilaisuuksia löytää kevyen unen hetki, ja luovuttaa enemmän aamustasi sen tekemiseen.

---

## 2. Mitä kello mittaa

Kellon terveyspalvelu tallentaa jokaiselta vuorokauden minuutilta **VMC**-luvun
("vector magnitude count", karkeasti kuinka paljon ranne liikkui sen minuutin aikana)
sekä karkean **asennon** (mihin suuntaan ranne osoittaa). Tämä tapahtuu koko ajan,
riippumatta siitä onko tämä sovellus käynnissä — ja juuri se tekee koko ratkaisun
mahdolliseksi: sovellus voi nukkua koko yön ja lukea silti koko yön jälkikäteen.

Tässä on oikea yö, täsmälleen sellaisena kuin kello sen tallensi — alla oleva data ei
ole havainnekuva, vaan yhden yön minuutit tekijän omasta kellosta:

![Liike yhden yön aikana, laukaisukynnys ja hetki jolloin herätys soi](img/night.fi.svg)

### Kynnys rakennetaan sinun omasta yöstäsi

Ei ole olemassa kiinteää "tämä määrä liikettä tarkoittaa hereillä oloa". Se mikä
lasketaan liikahdukseksi riippuu täysin siitä, kuinka liikkumatta olet ollut:

1. **Etsi mistä uni alkoi.** Käytetään kellon omaa unijaksoa jos sellainen on;
   muuten sovellus etsii ensimmäisen pitkän hiljaisen jakson. Sen jälkeiset ensimmäiset
   **20 minuuttia** heitetään pois — nukahtaminen ei edusta yötä.
2. **Ota kaikki siitä eteenpäin ikkunan alkuun asti** — ja *vain* ikkunan alkuun asti.
   Ikkuna on se mitä arvioidaan, joten sen päästäminen omaan vertailujoukkoonsa
   nostaisi rimaa joka kerta kun liikahdat.
3. **Pudota valvejaksot pois.** Mikä tahansa **vähintään 8 peräkkäisen minuutin**
   jakso, joka ylittää 4 × lepomediaanin (plus pieni kiinteä marginaali), ei ole unta
   vaan sinua hereillä — ja se poistetaan kokonaan. Yksi levoton tunti asettaisi
   muuten kynnyksen niin korkealle, ettei mikään myöhempi yöllä voisi ylittää sitä.
   Lyhyet purskeet säilytetään: tavallinen kääntyminen on dataa, ei poikkeama.
4. **Ota jäljelle jäävistä persentiili.** Se on **laukaisukynnys**. Yllä olevana yönä
   vertailujoukko oli 175 minuuttia, joista 86 % tasan nollia, ja 90. persentiiliksi
   tuli **587**.

Seuraus kannattaa sanoa suoraan: **hyvin rauhallisena yönä kynnys on matala, ja
tavallinen kääntyminen ylittää sen.** Se on suunnittelu toiminnassa, ei häiriö — mutta
se tarkoittaa, että rauhallinen nukkuja herätetään lähempänä ikkunan alkua kuin
levoton.

### Sekä voimakkuus että kesto

Kynnyksen ylittäminen yhden minuutin ajan ei riitä. Jokaisella ikkunan sisällä olevalla
minuutilla kertymä saa:

- **ylityksen**, jos minuutin liike on laukaisukynnyksen yläpuolella, ja
- **bonuksen 400**, jos ranne on kääntynyt — sen asento poikkeaa edeltävien kymmenen
  minuutin liukuvasta moodista vähintään 2 askelta 16:sta.

Minuutti joka ei tuota kumpaakaan **nollaa kertymän**. Herätys laukeaa kun kertymä on
saavuttanut yhden täyden laukaisukynnyksen *ja* tuottavien minuuttien sarja on
riittävän pitkä (1–5 minuuttia, oletus 2). Yksi piikki — yskäisy, kellon kolahdus
sängynpäätyyn — ei siis voi soittaa sitä, oli se kuinka suuri tahansa.

Erillistä vaimennusvakiota ei ole: koska sarja nollautuu millä tahansa hiljaisella
minuutilla, kertymä valuu tyhjäksi itsestään.

### Kaksi estoa

- **Jos laiteohjelmisto sanoo että olet juuri nyt syvässä unessa**, aikainen herätys
  estetään. Takaraja pätee silti.
- **Jos käyttökelpoisia minuutteja on alle 60**, joista rakentaa jakauma, älyherätys
  vetäytyy kokonaan ja herätysaika soittaa. Kello sanoo *"smart alarm unavailable"*
  sen sijaan että esittäisi tietävänsä.

### Herkkyys (Sensitivity)

Ainoa asia jonka tämä asetus muuttaa on se, mistä persentiilistä tulee laukaisukynnys:

| Asetus | Persentiili | Vaadittu kesto | Vaikutus |
|---|---|---|---|
| Low — only a clear stir | 95 | 2 min | Laukeaa harvoin; sinut herättää todennäköisemmin herätysaika itse |
| **Medium** *(oletus)* | 90 | 2 min | |
| High — the slightest stir | 82 | 2 min | Laukeaa aikaisin ja usein |
| Custom | oma valinta, 70–99 | oma valinta, 1–5 min | Ainoa tapa muuttaa vaadittua kestoa |

Kellon **Last night** -näyttö on olemassa juuri siksi, että tämän voi päättää eikä
arvata: se kertoo mikä laukaisukynnys oli, milloin herätys todella laukesi, ja
**milloin kukin muu herkkyys olisi laukaissut samana yönä**. Jos Low sanoo `--:--`,
se ei olisi laukaissut lainkaan — sinut olisi herättänyt herätysaika.

---

## 3. Miten se herättää sinut

![Miten värinä ja ääni voimistuvat soiton aikana](img/escalation.fi.svg)

Soitto on sarja **purskeita** — muutama värinäsykäys, sitten tauko, ja uudestaan. Ääni
liittyy mukaan myöhemmin, jos kellossa on kaiutin (vain osassa malleista on).

**Oletuksena värinä voimistuu.** Se aloittaa hennolla napautuksella ja tiukkenee
muutaman minuutin kuluessa, tasavälein, ja myös ääni voimistuu. Asetus **"Ramp the
vibration up"** pois päältä tekee värinästä täysivoimaisen heti ensimmäisestä
purskeesta, jolloin vain ääni voimistuu.

Kumpi herättää sinut paremmin on makuasia, ja kannattaa kokeilla molempia. Yksi
peruste tasaisen version puolesta on kirjattuna: toistuva hento ärsyke *voisi*
opettaa nukkujan sivuuttamaan sen kanavan, ja myös sen jälkeen tulevat voimakkaammat
sykäykset — päättelyä, ei mittausta, ja asetus on olemassa jotta nukkuja joka
havaitsee niin käyvän voi kytkeä sen pois.

**Wake style** -esiasetukset eroavat toisistaan tauon pituudessa, siinä kuinka kauan
soitto kestää ennen luovuttamista, milloin ääni liittyy mukaan ja kuinka kovaa siitä
tulee:

| Tyyli | Tauko purskeiden välillä | Ääni mukaan | Enimmäisvoimakkuus | Luovuttaa |
|---|---|---|---|---|
| Gentle | 45 s | 8 min | 70 | 20 min |
| **Normal** *(oletus)* | 30 s | 5 min | 100 | 15 min |
| Insistent | 15 s | 1 min | 100 | 15 min |
| Custom | kaikki kaksitoista lukua ovat omiasi | | | |

(Kaikki kaksitoista rinnakkain löytyvät luvusta 6 — esiasetukset eroavat vähemmän
kuin niiden nimet antavat ymmärtää.)

Kun soitto luovuttaa, se lakkaa pitämästä ääntä mutta jättää näytölle
**"Alarm missed"**, jotta aamu kertoo mitä tapahtui.

### Mitä soittoruudulla näkyy

Kellon ja kahden nappitekstin alla alarivi kertoo yön, joka tähän johti:
`Slept 1.7/6.5 h` — syvää unta tunteina kauttaviivalla unta yhteensä, samat kaksi
lukua jotka kellon oma terveysdata ja TimeStylen uni-widget näyttävät.
Torkkuruudulla on sama rivi, mutta kellon yläpuolella eikä nappitekstien alla. Jos kello ei ole
tallentanut lainkaan unta — terveysseuranta pois päältä, tai yö jota se ei nähnyt —
rivi puuttuu kokonaan eikä näytä nollia.

### Pysäyttäminen ja torkku

Soittoruudulla **kaikki napit vaativat kaksi painallusta**: ylänappi (*Snooze*,
jonka alla pieni rivi kertoo antamansa pituuden — `10 min`) torkuttaa kahdesti
painettuna, alanappi (*Stop*) pysäyttää kahdesti painettuna. Yksi
painallus ei koskaan tee kumpaakaan — se vain näyttää, mitä toinen painallus tekisi
(`Press 2x to snooze` / `Press 2x to stop`) — koska puoliunessa oleva käsi löytää
yhden napin tunnustelemalla, ja juuri niin herätys ennen saattoi tulla vahingossa
kuitatuksi tai torkutetuksi.

Torkku ei enää palauta suoraan kellotaululle. Sen sijaan kello pitää näytön auki:
kellonajan, `Snooze 1` (monesko torkku tämä on) ja `Rings again 09:40` (milloin se
soi seuraavan kerran), ja niiden alla pysyvän kaksirivisen muistutuksen — `2x Back:
watchface` ja `2x Down: cancel alarm`. **Kaksi painallusta DOWN-nappia peruu
herätyksen kokonaan** — se ei soi enää tänä yönä. Kaksi painallusta BACK-nappia vie
vain pois tältä näytöltä kellotaululle; torkku on yhä voimassa, ja herätys palaa
itsestään kun se päättyy. Jos sovellus suljetaan kokonaan torkun aikana (BACK
kahdesti, tai pitkä painallus, joka sulkee minkä tahansa sovelluksen tällä
kellolla), sen avaaminen uudelleen tuo saman näytön suoraan takaisin eikä valikkoa.
Torkun ollessa käynnissä sekä päävalikko että herätyslista kertovat sen sanoin —
`snoozed, in 4 min` — joten sitä ei tarvitse arvailla.

Torkku on oletuksena 10 minuuttia ja niitä sallitaan 5. **Jokainen torkku
aloittaa voimistumisen alusta.** (Voit muuttaa tämän: *"each snooze starts this far
along"* -liu'un nostaminen saa toisen herätyksen jatkamaan pidemmältä rampista — eli
alkamaan voimakkaampana kuin ensimmäinen.)

**Keskinappi tarjoaa toisen torkkupituuden**, myös toisella painalluksella: valikon
jossa on 5, 10, 15, 20, 30, 45 ja 60 minuuttia. Näytöllä sen merkkinä on pieni `+`
juuri sen napin korkeudella. Kun valikko on auki, valinta toimii yhdellä
painalluksella — valikon avaaminen oli se harkittu teko, ja erillinen varmistus
valikon sisällä, johon pääsi vain kaksoispainalluksella, olisi pelkkää seremoniaa.
Näin valittu pituus on kertaluonteinen: ylänappi tarkoittaa edelleen sitä, minkä
asetit puhelimella.

Kun torkut loppuvat — tai torkku on kytketty puhelimelta kokonaan pois — sekä ylä-
että keskinappi muuttuvat tehottomiksi, ja näyttö kertoo sen: `Snooze`-teksti,
sen alla oleva pituusrivi ja `+` katoavat kaikki, ja näytölle jää pelkkä `Stop`. Kaksi syytä. Nappia joka ei voi
tehdä mitään ei saa mainostaa sellaisena kuin se voisi; eikä ylänappi saa hiljaa
muuttua toiseksi pysäytykseksi, koska silloin kaksoispainallus, jolla halusi vain
muutaman minuutin lisää, sammuttaisikin koko herätyksen.

---

## 4. Kellon näytöt

Herätykset asetetaan puhelimella. Kello näyttää tilan ja tarjoaa pikatoimintoja, eikä
siinä ole mitään tapaa luoda herätysaikaa — tarkoituksella, koska puhelimen
näppäimistö voittaa neljä nappia.

**Valikko** — seuraava herätys ja kuinka kaukana se on, herätyslista, "Last night",
sekä Test alarm joka soi kahden minuutin päästä, jotta voit tuntea asetuksesi
odottamatta aamua.

**Herätyslista** — jokainen herätys ja sen tila sanoin (`in 23 h`, `skip, in 1 d`,
`off`). SELECT-painallus avaa pienen valikon, joka kirjoittaa auki mitä se tekee:
*Skip Mon 07:50* (vain tämä yksi kerta), *Turn off*, *Turn on*.

**Odotusnäyttö** — näkyy ikkunan ollessa auki, valkoisena mustalla jottei se ole
taskulamppu naamaan kello kolme yöllä. Siltä pääsee pois kahdella tavalla, ja
molemmat vaativat kaksi painallusta jottei kumpikaan tapahdu vahingossa:

- **DOWN kahdesti — peruu herätyksen.** Se ei soi, eikä se palaa.
- **BACK kahdesti — vie kellotaululle.** Ikkuna jää auki, joten näyttö palaa noin
  kolmen minuutin kuluessa.

**Ennakkonäyttö** — sama näyttö ja samat napit, jopa 90 minuuttia aiemmin. Aseta
puhelimella *"show alarm screen before"* (Off, 15, 30, 60 tai 90 minuuttia; oletus
tunti), ja kello nostaa odotusnäytön esiin sen verran ennen herätystä. Pointti on se
aamu, jona heräät itse jo 06:40: sen sijaan että makaisit odottamassa herätystä tai
etsisit valikoita pimeässä, näyttö on jo siellä ja kaksi DOWN-painallusta lopettaa
sen. Se kertoo mitä herätystä se odottaa, ja kun ajat eroavat, se nimeää molemmat —
`Alarm 07:00` ja sen alla `Rings by 07:30` — joten se ei koskaan ilmoita aikaa, joka
herätys ei ole.

Kolme asiaa siinä on tarkoituksellisia. Se **ei aloita älyikkunaa** — liikettä ei
vielä mitata, eikä herätyksen omiin herätyksiin kosketa. Se toimii **älyherätyksen
ollessa pois päältä**, jolloin se on ainoa näyttö jonka saat ennen soittoa. Ja **BACK
kahdesti poistuu lopullisesti**: toisin kuin odotusnäyttö, joka valvoo jotain ja
palaa kolmessa minuutissa, tämä ei valvo, joten sen sulkeminen tarkoittaa "anna olla
kunnes herätys on todella lähellä" — vaihtoehto olisi, että sovellus tunkisi itsensä
eteesi kolmisenkymmentä kertaa yhdeksänkymmenen minuutin aikana. Herätys itse pysyy
aseistettuna kummassakin tapauksessa.

**Soittonäyttö** — kellonaika, herätys, ja miten se pysäytetään (ks. §3).

---

## 5. Milloin sovellus on oikeasti käynnissä

![Milloin herätyssovellus todella käy yön aikana](img/process.fi.svg)

Pebble-sovelluksilla ei ole taustasäiettä: kerrallaan käy vain yksi sovellus, ja
kellotaulu on niistä yksi. Tämä sovellus *ei siis ole käynnissä* lähes koko yönä. Se
toimii pyytämällä kelloa herättämään itsensä tiettyinä hetkinä:

- **ikkunan avautuessa**, ja sen jälkeen kolmen minuutin välein ikkunan ollessa auki,
- **herätysaikana**, takarajana joka on aina laukeava,
- **kerran yössä klo 03:00**, pelkästään virittääkseen kaiken uudelleen mahdollisen
  kesäaikasiirtymän jälkeen.

Juuri siksi liikedata luetaan jälkikäteen laiteohjelmiston omasta historiasta eikä
näytteistetä livenä — ja siksi sovellus selviää siitä että se tapetaan, kello
käynnistyy uudelleen tai avaat jotain muuta: ajastetut herätykset ovat kellon
tallentamia, eivät sovelluksen muistissa. Se tarkoittaa myös, että kun sovellus *ei*
ole käynnissä, se katsoo liikettäsi kolmen minuutin välein eikä joka minuutti.

### Mikä voi silti mennä pieleen

| Tilanne | Mitä tapahtuu |
|---|---|
| Kello pois ranteesta, tai aktiivisuusseuranta pois päältä | Ei käyttökelpoista dataa → "smart alarm unavailable", herätysaika soittaa |
| Kello sammutettu herätyksen yli | Herätys jää väliin; kello kertoo siitä käynnistyessään |
| Puhelin poissa tai Bluetooth pois | Ei vaikutusta. Herätykset elävät kellossa; puhelinta tarvitaan vain niiden muokkaamiseen |
| Quiet Time päällä | **Ei vaikutusta värinään** — tämä on herätyskello, ja se sivuuttaa Quiet Timen tarkoituksella. Ääni voidaan silti vaimentaa, jos olet laittanut päälle "mute speaker during Quiet Time" |
| Akku riittävän tyhjä kellon virransäästötilaan | **Herätys ei voi soida.** Siinä tilassa kello sammuttaa ajastinherätyspalvelun kokonaan, ja jokainen sovelluksen herätys — tämä mukaan lukien — riippuu siitä. Kellon *sisäänrakennettu* herätys toimii yhä, koska se on kytketty selviämään tuosta tilasta eikä sovellus voi olla. Lataa kello, tai aseta sisäänrakennettu herätys varmistukseksi yönä jolla on väliä |

---

## 6. Jokainen asetus ja mitä se oikeasti muuttaa

*Custom*-merkityillä on merkitystä vain jos olet asettanut vastaavan valitsimen
Custom-tilaan; muuten esiasetus antaa nuo luvut.

### Alarms

| Asetus | Oletus | Mitä se muuttaa |
|---|---|---|
| Alarm list | yksi 07:00 herätys | Enintään 8 herätystä, kullakin aika, viikonpäivät ja on/off-kytkin |

### Smart alarm

| Asetus | Oletus | Mitä se muuttaa |
|---|---|---|
| Smart alarm | päällä | Pois päältä saa jokaisen herätyksen soimaan tasan aikanaan, eikä mikään alla olevasta merkitse mitään |
| Smart window length | 30 min | Kuinka paljon aamustasi tunnistin saa käyttää |
| Show alarm screen before | 60 min | Kuinka kauan ennen herätystä odotusnäyttö avautuu, jotta voit lopettaa herätyksen kahdella painalluksella jos heräät aikaisin. Off poistaa sen käytöstä. **Riippumaton älyherätyksestä** — toimii sen ollessa pois päältä, eikä koskaan aikaista soittoa |
| The alarm time is | the latest | Kummalle puolelle herätysaikaa ikkuna asettuu — katso luku 1 |
| Sensitivity | Medium | Mistä oman yösi persentiilistä tulee laukaisukynnys |
| *Custom:* stir percentile | 90 | Persentiili itse, 70–99 |
| *Custom:* sustained for | 2 min | Kuinka monta peräkkäistä tuottavaa minuuttia vaaditaan |

### How it wakes you

| Asetus | Oletus | Mitä se muuttaa |
|---|---|---|
| Ramp the vibration up | **päällä** | Päällä: hento aloitus joka tiukkenee. Pois: täysi voima ensimmäisestä purskeesta |
| Wake style | Normal | Esiasetus kaikille kahdelletoista alla olevalle luvulle |

**"Wake style" on täsmälleen nämä kaksitoista lukua**, ja Custom-valinta lähtee
liikkeelle Normal-sarakkeesta. Rinnakkain nähtynä ne ovat myös nopein tapa ymmärtää,
missä esiasetukset oikeasti eroavat toisistaan:

| *Custom*-asetus | Gentle | **Normal** | Insistent | Mitä se muuttaa |
|---|---|---|---|---|
| gap between buzzes | 45 s | 30 s | 15 s | Hiljaisuus purskeiden välillä alussa |
| final gap | 10 s | 5 s | 3 s | Hiljaisuus purskeiden välillä täysin voimistuneena (vain ramppi päällä) |
| tighten over | 600 s | 360 s | 180 s | Kuinka kauan voimistuminen kestää (vain ramppi päällä) |
| first pulse | 60 ms | 80 ms | 200 ms | Sykäyksen pituus alussa (vain ramppi päällä) |
| pulse length | 500 ms | 700 ms | 700 ms | Sykäyksen pituus voimistuneena — ja heti ensimmäisestä purskeesta kun ramppi on pois |
| first burst pulses | 1 | 1 | 2 | Sykäyksiä purskeessa alussa (vain ramppi päällä) |
| pulses per buzz | 3 | 3 | 3 | Sykäyksiä purskeessa voimistuneena — ja heti ensimmäisestä purskeesta kun ramppi on pois |
| sound joins after | 480 s | 300 s | 60 s | Milloin ääni alkaa. **Ei vaikutusta kellossa jossa ei ole kaiutinta** |
| volume ramp | 300 s | 300 s | 180 s | Kuinka kauan voimakkuudella kestää saavuttaa maksiminsa. Vain kaiuttimellinen |
| first volume | 10 | 15 | 30 | Voimakkuus kun ääni liittyy mukaan. Vain kaiuttimellinen |
| max volume | 70 | 100 | 100 | Kovin mihin se yltää. Vain kaiuttimellinen |
| give up after | 1200 s | 900 s | 900 s | Kuinka kauan soitto kestää ennen kuin se lakkaa pitämästä ääntä |

Sarakkeita alaspäin lukemalla esiasetukset ovat vähemmän erilaisia kuin niiden nimet
antavat ymmärtää — varsinkin ramppi pois päältä, mikä kytkee pois ne neljä riviä
jotka on merkitty *vain ramppi päällä* ja saa jokaisen purskeen alkamaan "pulse
length"- ja "pulses per buzz" -arvoista. Kaiuttimettomassa kellossa neljä riviä
lisää ei tee mitään, ja Gentlen ja Normalin ero kutistuu siihen, tuleeko identtisiä
täysvoimaisia purskeita 45 vai 30 sekunnin välein.

### Snooze and stopping

| Asetus | Oletus | Mitä se muuttaa |
|---|---|---|
| Snooze length | 10 min | Mitä ylänappi tarkoittaa: 5, 10, 15, 20, 30, 45 tai 60 min — samat pituudet jotka keskinapin valikko tarjoaa. Off poistaa torkun kokonaan — ylä- ja keskinappi eivät silloin tee mitään, ja vain alanappi (Stop) pysäyttää herätyksen |
| Total snooze time | 30 min | Kuinka monta MINUUTTIA torkkua saa käyttää yhteensä ennen kuin ylä- ja keskinappi muuttuvat tehottomiksi, riippumatta painallusten määrästä — ei siis painalluskertojen määrä. Unlimited on sallittu; soitto ei koskaan hiljene sen takia. Keskinapin valikko tarjoaa vain pituudet jotka vielä mahtuvat jäljellä olevaan aikaan |
| Each snooze starts this far along | 0 s | 0 aloittaa voimistumisen alusta joka kerta. Suurempi arvo saa jokaisen torkun alkamaan voimakkaampana |

---

## 7. Mitä se tarkoituksella jättää tekemättä

- **Se ei kunnioita Quiet Timea.** Herätyskello on olemassa soidakseen silloin kun
  kello on muuten hiljaa. Kellon oma rajapinta kutsuu tuota asetusta neuvoksi;
  herätyskellolle oikea vastaus on sivuuttaa se.
- **Se ei luokittele univaiheita.** Täällä ei ole REM/syvä/kevyt-mallia. Se mittaa
  liikettä, joka korreloi unen syvyyden kanssa riittävän hyvin hetken valitsemiseen,
  ja se sanoo niin sen sijaan että pukisi asian hienommaksi.
- **Se ei käytä kiihtyvyysanturia suoraan.** Se vaatisi sovelluksen olevan käynnissä
  koko yön, mitä Pebble ei salli eikä akkusi kestäisi. Laiteohjelmiston oma
  minuuttihistoria on sekä halvempi että kattavampi.
- **Se ei luo herätyksiä kellossa**, kahden minuutin Test alarmia lukuun ottamatta.

---

## Liite — mistä luvut ovat peräisin

Yökaavio on oikeaa dataa, ja sen voi tarkistaa: `img/night-2026-08-01.txt` sisältää
ne 640 minuuttikohtaista arvoa jotka kello tallensi, ja `img/make_diagrams.py`
piirtää jokaisen tämän dokumentin kaavion uudelleen tuosta tiedostosta ja
lähdekoodin vakioista. Kaavioon piirretty kynnys (587) on se minkä kello itse laski
ja kirjasi sinä aamuna, ja 90. persentiilin laskeminen uudelleen julkaistusta
datasta tuottaa täsmälleen saman luvun.

Asetusten nimet on jätetty englanniksi, koska puhelimen asetussivu on englanniksi:
näin nimi jonka luet täältä on sama jonka näet puhelimessa.
