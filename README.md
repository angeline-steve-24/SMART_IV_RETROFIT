Smart IV Retrofit

An IoT-Based IV Bag Weight and Level Monitoring System for Gravity-Based IV Therapy

Smart IV Retrofit is an embedded IoT-based monitoring system designed to continuously monitor the fluid level of a conventional gravity-based IV bag. The system uses a load cell and HX711 amplifier to measure the weight of the IV bag, while an ESP32 processes the measurements and provides local and wireless monitoring.

Features
Real-time IV bag weight monitoring
Remaining fluid volume estimation
Remaining fluid percentage calculation
Flow-rate estimation based on weight change
Approximate remaining infusion time
Possible flow-interruption detection
16×2 I2C LCD display
Buzzer-based fluid-level alerts
ESP32 Wi-Fi local web dashboard
Real-time monitoring through a web browser
Hardware Components
ESP32 Development Board
5 kg Load Cell
HX711 Load Cell Amplifier
16×2 I2C LCD
Buzzer
IV Bag and IV Stand
Connecting Wires
5V Power Supply
Pin Connections
Component	ESP32 Pin
HX711 DOUT	GPIO 23
HX711 SCK	GPIO 19
Buzzer	GPIO 4
LCD	I2C
LCD Address	0x27
Working Principle

The IV bag is suspended from a load cell mounted on the IV stand. As the IV fluid is consumed, the weight of the bag decreases. The HX711 acquires the load-cell signal and sends the measurement to the ESP32.

The ESP32 processes the measured weight to determine the remaining fluid quantity and percentage. Changes in fluid quantity over time are used to estimate the flow rate and remaining infusion time. If the measured quantity remains unchanged for a predefined period, the system indicates a possible flow interruption.

The LCD provides local information, while the buzzer provides audible alerts. The ESP32 also creates a local Wi-Fi network and hosts a web dashboard for real-time monitoring.

Software
Arduino IDE
Embedded C/C++
ESP32 Arduino Core
HX711 Library
LiquidCrystal_I2C Library
ESP32 Wi-Fi
WebServer Library
Calibration

The load cell must be calibrated using the actual hardware before monitoring.

Hang the empty IV bag on the load cell.
Record the empty-bag reading.
Hang the full 500 mL IV bag.
Record the full-bag reading.
Store the calibration values in the ESP32.
Start real-time monitoring.

Calibration should be performed whenever the mechanical setup or load-cell configuration is changed.

Web Dashboard

After starting the ESP32, connect a device to the local Wi-Fi network:

Network: IV_MONITOR
Password: 12345678

Then open:

http://192.168.4.1

The dashboard displays:

Remaining IV volume
Fluid percentage
Flow rate
Estimated remaining time
IV status
Possible flow interruption
Project Structure
Smart-IV-Retrofit/
│
├── Smart_IV_Retrofit.ino
├── README.md
├── Circuit-Diagram/
├── Prototype-Images/
└── Documentation/
Project Objective

The main objective of this project is to enhance a conventional gravity-based IV setup with embedded sensing, real-time monitoring, alerts, and local IoT connectivity without changing the basic mechanism of IV fluid delivery.

Scope

This project is developed as an academic prototype for IV fluid monitoring and alert generation. It does not automatically control or regulate the IV fluid flow and is not intended to replace clinically approved medical equipment.

Team

Project: Smart IV Retrofit
Department: Computer Science and Engineering
Institution: Chennai Institute of Technology
Course: Embedded Programming (CS4502)
Project Type: Project-Based Learning (PBL)
