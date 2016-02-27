#include <user_config.h>
#include <SmingCore.h>
#include <SmingCore/Network/TelnetServer.h>
#include <AppSettings.h>
#include <globals.h>
#include <i2c.h>
#include <IOExpansion.h>
#include <RTClock.h>
#include <Wifi.h>
#include <SDCard.h>
#include <MyGateway.h>

#if CONTROLLER_TYPE == CONTROLLER_TYPE_OPENHAB
  #include <openHabMqttController.h>
  OpenHabMqttController controller;
#elif CONTROLLER_TYPE == CONTROLLER_TYPE_CLOUD
  #include <CloudController.h>
  CloudController controller;  
#else
  #error Wrong CONTROLLER_TYPE value
#endif

#include <Libraries/SD/SD.h>

// set up variables using the SD utility library functions:
Sd2Card card;
SdVolume volume;
SdFile root;

/*
 * The I2C implementation takes care of initializing things like I/O
 * expanders, the RTC chip and the OLED.
 */
MyI2C I2C_dev;
RTClock rtcDev;

HttpServer server;
FTPServer ftp;
TelnetServer telnet;
static boolean first_time = TRUE;

char convBuf[MAX_PAYLOAD*2+1];

void incomingMessage(const MyMessage &message)
{
    // Pass along the message from sensors to serial line
    Debug.printf("APP RX %d;%d;%d;%d;%d;%s\n",
                  message.sender, message.sensor,
                  mGetCommand(message), mGetAck(message),
                  message.type, message.getString(convBuf));

    if (GW.getSensorTypeString(message.type) == "VAR2") 
    {
        Debug.printf("received pong\n");
    }

    if (mGetCommand(message) == C_SET)
    {
        String type = GW.getSensorTypeString(message.type);
        String topic = message.sender + String("/") +
                       message.sensor + String("/") +
                       "V_" + type;
        controller.notifyChange(topic, message.getString(convBuf));
    }

    return;
}

Timer reconnectTimer;
void wifiConnect()
{
    WifiStation.enable(false);
    WifiStation.enable(true);

    if (AppSettings.ssid.equals("") &&
        !WifiStation.getSSID().equals(""))
    {
        AppSettings.ssid = WifiStation.getSSID();
        AppSettings.password = WifiStation.getPassword();
        AppSettings.save();
    }

    WifiStation.config(AppSettings.ssid, AppSettings.password);
    if (!AppSettings.dhcp && !AppSettings.ip.isNull())
    {
        WifiStation.setIP(AppSettings.ip,
                          AppSettings.netmask,
                          AppSettings.gateway);
    }
}

Timer softApSetPasswordTimer;
void apEnable()
{
     Wifi.softApEnable();
}

void onIpConfig(HttpRequest &request, HttpResponse &response)
{
    if (request.getRequestMethod() == RequestMethod::POST)
    {
        String oldApPass = AppSettings.apPassword;
        AppSettings.apPassword = request.getPostParameter("apPassword");
        if (!AppSettings.apPassword.equals(oldApPass))
        {
            softApSetPasswordTimer.initializeMs(500, apEnable).startOnce();
        }

        AppSettings.ssid = request.getPostParameter("ssid");
        AppSettings.password = request.getPostParameter("password");
        AppSettings.portalUrl = request.getPostParameter("portalUrl");
        AppSettings.portalData = request.getPostParameter("portalData");

        AppSettings.dhcp = request.getPostParameter("dhcp") == "1";
        AppSettings.ip = request.getPostParameter("ip");
        AppSettings.netmask = request.getPostParameter("netmask");
        AppSettings.gateway = request.getPostParameter("gateway");
        Debug.printf("Updating IP settings: %d", AppSettings.ip.isNull());
        AppSettings.save();
    
        reconnectTimer.initializeMs(10, wifiConnect).startOnce();
    }

    TemplateFileStream *tmpl = new TemplateFileStream("settings.html");
    auto &vars = tmpl->variables();

    vars["ssid"] = AppSettings.ssid;
    vars["password"] = AppSettings.password;
    vars["apPassword"] = AppSettings.apPassword;

    vars["portalUrl"] = AppSettings.portalUrl;
    vars["portalData"] = AppSettings.portalData;

    bool dhcp = WifiStation.isEnabledDHCP();
    vars["dhcpon"] = dhcp ? "checked='checked'" : "";
    vars["dhcpoff"] = !dhcp ? "checked='checked'" : "";

    if (!WifiStation.getIP().isNull())
    {
        vars["ip"] = WifiStation.getIP().toString();
        vars["netmask"] = WifiStation.getNetworkMask().toString();
        vars["gateway"] = WifiStation.getNetworkGateway().toString();
    }
    else
    {
        vars["ip"] = "0.0.0.0";
        vars["netmask"] = "255.255.255.255";
        vars["gateway"] = "0.0.0.0";
    }

    response.sendTemplate(tmpl); // will be automatically deleted
}

void onFile(HttpRequest &request, HttpResponse &response)
{
    static bool alreadyInitialized = false;

    String file = request.getPath();
    if (file[0] == '/')
        file = file.substring(1);

    if (file[0] == '.')
        response.forbidden();
    else
    {
        response.setCache(86400, true); // It's important to use cache for better performance.

        // open the file. note that only one file can be open at a time,
        // so you have to close this one before opening another.
        Debug.printf("REQUEST for %s\n", file.c_str());
        if (alreadyInitialized || SD.begin(0))
        {
            alreadyInitialized = true;
            Debug.printf("SD card present\n");
            File f = SD.open(file);
            if (!f) //SD.exists(file) does not seem to work
            {
                Debug.printf("NOT FOUND %s\n", file.c_str());
                response.sendFile(file);
            }
            else if (f.isDirectory())
            {
                f.close();
                Debug.printf("%s IS A DIRECTORY\n", file.c_str());
                response.forbidden();
            }
            else
            {
                f.close();
                Debug.printf("OPEN STREAM FOR %s\n", file.c_str());
                response.setAllowCrossDomainOrigin("*");
                const char *mime = ContentType::fromFullFileName(file);
		if (mime != NULL)
		    response.setContentType(mime);
                SdFileStream *stream = new SdFileStream(file);
                response.sendDataStream(stream);
            }
        }
        else
        {
            response.sendFile(file);
        }
    }
}

void startWebServer()
{
    server.listen(80);
    server.addPath("/", onIpConfig);
    server.addPath("/ipconfig", onIpConfig);

    GW.registerHttpHandlers(server);
    controller.registerHttpHandlers(server);
    server.setDefaultHandler(onFile);
}

void startFTP()
{
    if (!fileExist("index.html"))
        fileSetContent("index.html", "<h3>Please connect to FTP and upload files from folder 'web/build' (details in code)</h3>");

    // Start FTP server
    ftp.listen(21);
    ftp.addUser("me", "123"); // FTP account
}

int updateSensorStateInt(int node, int sensor, int type, int value)
{
  MyMessage myMsg;
  myMsg.set(value);
  GW.sendRoute(GW.build(myMsg, node, sensor, C_SET, type, 0));
}

int updateSensorState(int node, int sensor, int value)
{
  updateSensorStateInt(node, sensor, 2 /*message.type*/, value);
}

Timer heapCheckTimer;
void heapCheckUsage()
{
    controller.notifyChange("memory", String(system_get_free_heap_size()));    
}

uint64_t rfBaseAddress;

void i2cChangeHandler(String object, String value)
{
    controller.notifyChange(object, value);
}

// Will be called when system initialization was completed
void startServers()
{
    I2C_dev.begin(i2cChangeHandler);
    Expansion.begin(i2cChangeHandler);
    rtcDev.begin(i2cChangeHandler);

    heapCheckTimer.initializeMs(60000, heapCheckUsage).start(true);

    startFTP();
    startWebServer();
    telnet.listen(23);

    if (AppSettings.useOwnBaseAddress)
    {
        rfBaseAddress = ((uint64_t)system_get_chip_id()) << 8;
    }
    else
    {
        rfBaseAddress = RF24_BASE_RADIO_ID;
    }

    controller.begin();
}

void ntpTimeResultHandler(NtpClient& client, time_t ntpTime)
{
    SystemClock.setTime(ntpTime, eTZ_UTC);
    Debug.print("Time after NTP sync: ");
    Debug.println(SystemClock.getSystemTimeString());
    rtcDev.setTime(ntpTime);
}

NtpClient ntpClient(NTP_DEFAULT_SERVER, 0, ntpTimeResultHandler);

// Will be called when WiFi station was connected to AP
void wifiCb(bool connected)
{
    if (connected)
    {
        Debug.println("--> I'm CONNECTED");
        if (first_time) 
        {
            first_time = FALSE;
            // start getting sensor data
            GW.begin(incomingMessage, NULL, rfBaseAddress);
            ntpClient.requestTime();
        }
    }
}

// change this to match your SD shield or module;
// Arduino Ethernet shield: pin 4
// Adafruit SD shields and modules: pin 10
// Sparkfun SD shield: pin 8
const int chipSelect = 0;
void processSD(String commandLine, CommandOutput* out)
{
  out->printf("Initializing SD card...\n");

  // we'll use the initialization code from the utility libraries
  // since we're just testing if the card is working!
  if (!card.init(SPI_HALF_SPEED, chipSelect)) {
    out->printf("initialization failed. Things to check:\n");
    out->printf("* is a card inserted?\n");
    out->printf("* is your wiring correct?\n");
    out->printf("* did you change the chipSelect pin to match your shield or module?\n");
    return;
  } else {
    out->printf("Wiring is correct and a card is present.\n");
  }

  // print the type of card
  out->printf("Card type: ");
  switch (card.type()) {
    case SD_CARD_TYPE_SD1:
      out->printf("SD1\n");
      break;
    case SD_CARD_TYPE_SD2:
      out->printf("SD2\n");
      break;
    case SD_CARD_TYPE_SDHC:
      out->printf("SDHC\n");
      break;
    default:
      out->printf("Unknown\n");
  }

  // Now we will try to open the 'volume'/'partition' - it should be FAT16 or FAT32
  if (!volume.init(card)) {
    out->printf("Could not find FAT16/FAT32 partition.\nMake sure you've formatted the card\n");
    return;
  }


  // print the type and size of the first FAT-type volume
  uint32_t volumesize;
  out->printf("Volume type is FAT%d\n", volume.fatType());

  volumesize = volume.blocksPerCluster();    // clusters are collections of blocks
  volumesize *= volume.clusterCount();       // we'll have a lot of clusters
  volumesize *= 512;                            // SD card blocks are always 512 bytes
  out->printf("Volume size (bytes): %d\n", volumesize);
  out->printf("Volume size (Kbytes): ");
  volumesize /= 1024;
  out->printf("%d\n", volumesize);
  out->printf("Volume size (Mbytes): ");
  volumesize /= 1024;
  out->printf("%d\n", volumesize);


  Serial.println("\nFiles found on the card (name, date and size in bytes): ");
  root.openRoot(volume);

  // list all files in the card with date and size
  root.ls(LS_R | LS_DATE | LS_SIZE);
}

void processInfoCommand(String commandLine, CommandOutput* out)
{
    out->printf("System information : ESP8266 based MySensors gateway\r\n");
    out->printf("Build time         : %s\n", build_time);
    out->printf("Version            : %s\n", build_git_sha);
    out->printf("Sming Version      : %s\n", SMING_VERSION);
    out->printf("ESP SDK version    : %s\n", system_get_sdk_version());
    out->printf("MySensors version  : %s\n", GW.version());
    out->printf("\r\n");
    out->printf("Time               : ");
    out->printf(SystemClock.getSystemTimeString().c_str());
    out->printf("\r\n");
    out->printf("Free Heap          : %d\r\n", system_get_free_heap_size());
    out->printf("CPU Frequency      : %d MHz\r\n", system_get_cpu_freq());
    out->printf("System Chip ID     : %x\r\n", system_get_chip_id());
    out->printf("SPI Flash ID       : %x\r\n", spi_flash_get_id());
    out->printf("SPI Flash Size     : %d\r\n", (1 << ((spi_flash_get_id() >> 16) & 0xff)));
    out->printf("\r\n");
    out->printf("RF base address    : %02x", (rfBaseAddress >> 32) & 0xff);
    out->printf("%08x\r\n", rfBaseAddress);
    out->printf("\r\n");
}

void processRestartCommand(String commandLine, CommandOutput* out)
{
    System.restart();
}

void processDebugCommand(String commandLine, CommandOutput* out)
{
    Vector<String> commandToken;
    int numToken = splitString(commandLine, ' ' , commandToken);

    if (numToken != 2 ||
        (commandToken[1] != "on" && commandToken[1] != "off"))
    {
        out->printf("usage : \r\n\r\n");
        out->printf("debug on  : Enable debug\r\n");
        out->printf("debug off : Disable debug\r\n");
        return;
    }

    if (commandToken[1] == "on")
        Debug.start();
    else
        Debug.stop();
}

void processCpuCommand(String commandLine, CommandOutput* out)
{
    Vector<String> commandToken;
    int numToken = splitString(commandLine, ' ' , commandToken);

    if (numToken != 2 ||
        (commandToken[1] != "80" && commandToken[1] != "160"))
    {
        out->printf("usage : \r\n\r\n");
        out->printf("cpu 80  : Run at 80MHz\r\n");
        out->printf("cpu 160 : Run at 160MHz\r\n");
        return;
    }

    if (commandToken[1] == "80")
    {
        System.setCpuFrequency(eCF_80MHz);
        AppSettings.cpuBoost = false;
    }
    else
    {
        System.setCpuFrequency(eCF_160MHz);
        AppSettings.cpuBoost = true;
    }

    AppSettings.save();
}

void processBaseAddressCommand(String commandLine, CommandOutput* out)
{
    Vector<String> commandToken;
    int numToken = splitString(commandLine, ' ' , commandToken);

    if (numToken != 2 ||
        (commandToken[1] != "default" && commandToken[1] != "private"))
    {
        out->printf("usage : \r\n\r\n");
        out->printf("base-address default : Use the default MySensors base address\r\n");
        out->printf("base-address private : Use a base address based on ESP chip ID\r\n");
        return;
    }

    if (commandToken[1] == "default")
    {
        AppSettings.useOwnBaseAddress = false;
    }
    else
    {
        AppSettings.useOwnBaseAddress = true;
    }

    AppSettings.save();
    System.restart();
}

void ping(void)
{
    int node = 22;
    int sensor = 1; 
    String type = "VAR1";
    int message = 1;
    /* send ping */
    updateSensorStateInt(node, sensor,
                         MyGateway::getSensorTypeFromString(type),
                         message);
}

Timer pingpongTimer;
void processPongCommand(String commandLine, CommandOutput* out)
{
    Vector<String> commandToken;
    int numToken = splitString(commandLine, ' ' , commandToken);

    if (numToken != 2 ||
        (commandToken[1] != "start" && commandToken[1] != "stop"))
    {
        out->printf("usage : \r\n\r\n");
        out->printf("pong stop : stop ping-pong node test\r\n");
        out->printf("pong start : start ping-pong node test\r\n");
        return;
    }

    if (commandToken[1] == "start")
    {
        pingpongTimer.initializeMs(500, ping).start();
    }
    else
    {
        pingpongTimer.initializeMs(500, ping).stop();
    }
}

void processShowConfigCommand(String commandLine, CommandOutput* out)
{
    out->println(fileGetContent(".settings.conf"));
}

#include "ScriptCore.h"
void printHandler(CScriptVar *v, void *userdata)
{
    String a1 =  v->getParameter("arg1")->getString();
    Debug.println(a1);
}

bool first = true;
void processJS(String commandLine, CommandOutput* out)
{
    if (first)
    {
        ScriptingCore.addNative("function print(arg1)", printHandler, NULL);
        first = false;
    }

    ScriptingCore.execute("SetObjectValue(\"sensor2\", \"1\");");
    ScriptingCore.execute("result = 1; print(\"All Done \"+result);");
    bool pass = ScriptingCore.root->getParameter("result")->getBool();
    out->printf("Result: %s\n", pass ? "true" : "false");
    ScriptingCore.execute("print(\"GetObjectValue() => \"+GetObjectValue(\"sensor1\"));");
    //ScriptingCore.execute("for (result=0; result<100; result++) { print(result); }");
    ScriptingCore.execute("SetObjectValue(\"sensor2\", \"0\");");
}

void init()
{
    /* Mount the internal storage */
    int slot = rboot_get_current_rom();
    if (slot == 0)
    {
        Debug.printf("trying to mount spiffs at %x, length %d\n",
                     0x40300000, SPIFF_SIZE);
        spiffs_mount_manual(0x40300000, SPIFF_SIZE);
    }
    else
    {
        Debug.printf("trying to mount spiffs at %x, length %d\n",
                     0x40500000, SPIFF_SIZE);
        spiffs_mount_manual(0x40500000, SPIFF_SIZE);
    }

    Serial.begin(SERIAL_BAUD_RATE); // 115200 by default
    Serial.systemDebugOutput(true); // Enable debug output to serial
    Serial.commandProcessing(true);
    Debug.start();

    // set prompt
    commandHandler.setCommandPrompt("MySensorsGateway > ");

    // add new commands
    commandHandler.registerCommand(CommandDelegate("info",
                                                   "Show system information",
                                                   "System",
                                                   processInfoCommand));
    commandHandler.registerCommand(CommandDelegate("debug",
                                                   "Enable or disable debugging",
                                                   "System",
                                                   processDebugCommand));
    commandHandler.registerCommand(CommandDelegate("restart",
                                                   "Restart the system",
                                                   "System",
                                                   processRestartCommand));
    commandHandler.registerCommand(CommandDelegate("cpu",
                                                   "Adjust CPU speed",
                                                   "System",
                                                   processCpuCommand));
    commandHandler.registerCommand(CommandDelegate("sd",
                                                   "Test SD",
                                                   "System",
                                                   processSD));
    commandHandler.registerCommand(CommandDelegate("js",
                                                   "Test JS",
                                                   "System",
                                                   processJS));
    commandHandler.registerCommand(CommandDelegate("showConfig",
                                                   "Show the current configuration",
                                                   "System",
                                                   processShowConfigCommand));
    commandHandler.registerCommand(CommandDelegate("base-address",
                                                   "Set the base address to use",
                                                   "MySensors",
                                                   processBaseAddressCommand));
    commandHandler.registerCommand(CommandDelegate("pong",
                                                   "link quality test",
                                                   "MySensors",
                                                   processPongCommand));
    AppSettings.load();

    Wifi.begin(wifiCb);

    // CPU boost
    if (AppSettings.cpuBoost)
        System.setCpuFrequency(eCF_160MHz);
    else
        System.setCpuFrequency(eCF_80MHz);

   #if MEASURE_ENABLE
   pinMode(SCOPE_PIN, OUTPUT);
   #endif
	
    // Run WEB server on system ready
    System.onReady(startServers);
}
