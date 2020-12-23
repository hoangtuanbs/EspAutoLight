# EspAutoLight

An automatic light controller that turns ON light when dark. It
uses current time/location as indication rather than light sensors.

It uses timescheduler for scheduling work:
- MQTT Update: Publish status to a local broker every minute:
  {"time":"15:18:30","relays":["OFF"],"message":"Sunset: Sunset: 4:02:30 PM"}
- Display current status to a ePaper 2.9 inch
- Check sunset via http://api.sunrise-sunset.org daily
- It turns ON/OFF relay at LED pin between sunset and 24:00
- Firmware can be updated via OTA, password protected
