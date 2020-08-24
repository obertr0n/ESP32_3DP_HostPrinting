#include <functional>
#include <SPIFFS.h>

#include "WiFi_Manager.h"
#include "Config.h"
#include "Log.h"
#include "Util.h"

using namespace std;

void WiFiManagerClass::begin()
{
    SPIFFS.begin();
    _pref.begin(PREF_WIFI_NAMESPACE.c_str());

    _ssid = getStringPref(PREF_KEY_WIFI_SSID);
    _pass = getStringPref(PREF_KEY_WIFI_PASS);
    _wifiMode = (WiFiMode_t)getWifiModePref(PREF_KEY_WIFI_MODE);

    /* check how we should start based on Preference */
    if(WIFI_AP == _wifiMode)
    {
        startAP();
    }
    else if(WIFI_STA == _wifiMode)
    {
        /* check if stored details are working OK */
        if(!startSTA())
        {
            beginCaptive();
            loopCaptive();
        }
    }
}

void WiFiManagerClass::resetSetting()
{
    /* just by setting this to STA mode, we should restart the portal */
    _wifiMode = WIFI_STA;
    setWifiModePref(PREF_KEY_WIFI_MODE, (uint8_t)_wifiMode);

    /* now we  reboot */
    Util.sysReboot();
};

void WiFiManagerClass::beginCaptive()
{
    /* if we are here, we either failed WiFi connection */
    /* start in AP mode */
    LOG_Println("Captive starting...");
    /* ensure correct Wifi statup */
    WiFi.disconnect(true, true);
    startAP();
    
    _server = new AsyncWebServer(80);
    _dns = new DNSServer();

    _dns->setErrorReplyCode(DNSReplyCode::NoError);
    /* any request will be captured */
    _dns->start(DNS_PORT, "*", WiFi.softAPIP());

    _server->onNotFound(bind(&WiFiManagerClass::webServerHandleNotFound, this, placeholders::_1));
    _server->on("/", HTTP_GET, bind(&WiFiManagerClass::webServerGETRoot, this, placeholders::_1));
    _server->on("/www/captive.css", HTTP_GET, bind(&WiFiManagerClass::webServerGETLoadCSS, this, placeholders::_1));
    _server->on("/reqwifi", HTTP_ANY, bind(&WiFiManagerClass::webServerANYWifReq, this, placeholders::_1));

    _server->begin();
}

void WiFiManagerClass::webServerHandleNotFound(AsyncWebServerRequest *request)
{
    LOG_Println("In not found" + request->url());
    if(request->url() != "/")
    {
        AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Location", String("http://") + request->client()->localIP().toString());
        request->send(response);
    }
}

void WiFiManagerClass::webServerGETLoadCSS(AsyncWebServerRequest *request)
{
    AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/www/captive.css.gz", "text/css");
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void WiFiManagerClass::webServerGETRoot(AsyncWebServerRequest *request)
{
    AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/www/captive.html.gz", "text/html");
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void WiFiManagerClass::webServerANYWifReq(AsyncWebServerRequest *request)
{
    int code = 403;
    String result;

    LOG_Println("Serve req");
    if (request->hasArg("refresh"))
    {
        result = "{\"networks\":[]}";
        
        int16_t scanRet = WiFi.scanNetworks();

        if(scanRet == WIFI_SCAN_FAILED)
        {
            LOG_Println("Scan Failed");
        }
        else if(scanRet > 0)
        {
            result = "{\"networks\":[";
            for (int16_t netId = 0; netId < scanRet; netId++)
            {
                result += "{\"ssid\":\"" + WiFi.SSID(netId) + "\",";
                result += "\"rssi\":\"" + static_cast<String>(WiFi.RSSI(netId)) + "\",";
                result += "\"chan\":\"" + static_cast<String>(WiFi.channel(netId)) + "\",";
                result += "\"type\":\"" + static_cast<String>(WiFi.encryptionType(netId)) + "\",";
                result += "\"bssid\":\"" + WiFi.BSSIDstr(netId) + "\"},";
            }
            result[result.length() - 1] = ']';
            result += "}";
        }
        code = 200;
        LOG_Println(result);
    }
    else if(request->hasArg("mode"))
    {
        String mode = request->arg("mode");
        if(mode == "ap")
        {
            _wifiMode = WIFI_AP;
            _doReset = true;
        }
        else if(mode == "sta")
        {
            _wifiMode = WIFI_STA;
        }
        setWifiModePref(PREF_KEY_WIFI_MODE, (uint8_t)_wifiMode);
        code = 200;
        result = "OK";
    }
    else if(2 == request->args())
    {
        _ssid = request->arg((size_t)0);
        if (_ssid.length() > 0)
        {
            _needConfig = true;
        }
        _pass = request->arg((size_t)1);
        code = 200;
        result = "OK";
    }
    request->send(code, "text/plain", result);
}

void WiFiManagerClass::startAP()
{
    LOG_Println("Starting in AP mode");
    WiFi.softAP(AP_DEFAULT_SSID, AP_DEFAULT_PASS);
    delay(2000);
    WiFi.softAPConfig(_softApIP, _softApIP, _softApSnet);
    WiFi.begin();
}

bool WiFiManagerClass::startSTA()
{
    const char* pass = NULL;
    
    LOG_Println("called with SSID " + _ssid + " and pass " + _pass);

    if (_pass.length() > 0)
    {
        pass = _pass.c_str();
    }
    
    if (_ssid.length() > 0)
    {
        LOG_Println("using " + _ssid + " and pass " + _pass);

        if (!WiFi.mode(WIFI_STA))
        {
            return false;
        }
        if(WL_CONNECT_FAILED == WiFi.begin(_ssid.c_str(), pass))
        {
            return false;
        }

        uint32_t timeout = millis() + CONNECTION_TIMEOUT;

        while ((WiFi.status() != WL_CONNECTED) &&
            (timeout > millis()))
        {
            LOG_Println(".");
            delay(500);
        }
        /* connection failed or timmedout */
        if(WiFi.status() == WL_DISCONNECTED)
        {
            LOG_Println("Conn FAIL");
            return false;
        }
        else if(WiFi.status() == WL_CONNECTED)
        {
            LOG_Println("Conn OK");
            return true;
        }
    }
    return false;
}

void WiFiManagerClass::setStringPref(String const &key, String const &val)
{
    _pref.putString(key.c_str(), val);
}

String WiFiManagerClass::getStringPref(String const &key)
{
    return _pref.getString(key.c_str(), "");
}

void WiFiManagerClass::setWifiModePref(String const &key, uint8_t val)
{
    _pref.putUChar(key.c_str(), val);
}

uint8_t WiFiManagerClass::getWifiModePref(String const &key)
{
    return _pref.getUChar(key.c_str(), WIFI_STA);
}

void WiFiManagerClass::loopCaptive()
{
    for(;;)
    {
        if(_needConfig)
        {
            LOG_Println("test new connection");
            /* connected successfully */
            if(startSTA())
            {
                _wifiMode = WIFI_STA;

                setStringPref(PREF_KEY_WIFI_SSID, _ssid);
                setStringPref(PREF_KEY_WIFI_PASS, _pass);
                setWifiModePref(PREF_KEY_WIFI_MODE, (uint8_t)_wifiMode);

                /* setting saved, reset */
                _doReset = true;
            }
            _needConfig = false;
        }

        if(_doReset)
        {
            _doReset = false;
            Util.sysReboot();
        }
        _dns->processNextRequest();
    }
}

WiFiManagerClass WiFiManager;