/*
  "Elevator"

You need to develop a program – an elevator emulator. At startup, the program should request the number of floors in the building. The program should display the current state of the elevator to the user (floor number, direction of movement (up, down, stopped)). Elevator movement speed: 1 floor per second. The user should be able to "press" the call button on any floor at any time, including during elevator movement periods. During movement, any number of "call" buttons on different floors can be pressed. The user should be able to "press" the button for any floor on the call panel inside the elevator cabin.

When writing the program, you should be guided by your own common sense and knowledge about how elevators work. All other restrictions on the program's operation should be determined by the programmer at their own discretion. No additional clarifications on the task will be provided.

Requirements:

    Program interface – text-based.

    Example of program operation:
    // Initial conditions (elevator is on the 3rd floor, no passengers)

        F01 //call elevator to the first floor (Floor 01)
        Cabin on floor 3. Going down. <Press any key to enter new command>
        Cabin on floor 2. Going down <Press any key to enter new command>
        Cabin on floor 1. Stopped. Doors opened
        C4 //person entered the elevator and pressed the button for the 4th floor (Cabin 04)
        Lift Cabin on floor 1. Going up. <Press any key to enter new command> etc.

    The program must be written in C++ and compile with the GCC compiler

    The program's source code must be stored under Git version control system on the GitHub server or any other free repository

    The candidate must provide a compiled executable file for Windows PC or Linux PC and a link to the repository with the source code

    The program code must be neat, structured, and supplied with comments (in reasonable volume)
*/
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <atomic>

using namespace std;
using namespace std::chrono;

enum Direction { UP, DOWN, STOPPED };

class Elevator {
private:
    int currentFloor = 1;
    int maxFloors;
    Direction dir = STOPPED;
    vector<int> targets;
    mutex mtx;
    atomic<bool> running{true};

public:
    Elevator(int floors) : maxFloors(floors) {}

    void addTarget(int floor) {
        if (floor < 1 || floor > maxFloors) return;
        lock_guard<mutex> lock(mtx);
        if (find(targets.begin(), targets.end(), floor) == targets.end()) {
            targets.push_back(floor);
            sort(targets.begin(), targets.end());
        }
    }

    // РњРµС‚РѕРґ РґР»СЏ СЌРєСЃС‚СЂРµРЅРЅРѕР№ РѕСЃС‚Р°РЅРѕРІРєРё
    void emergencyStop() {
        lock_guard<mutex> lock(mtx);
        targets.clear();
        dir = STOPPED;
        cout << "\n!!! EMERGENCY STOP: All targets cleared. Cabin halted at floor " << currentFloor << " !!!" << endl;
    }

    void run() {
        while (running) {
            int nextTarget = -1;
            {
                lock_guard<mutex> lock(mtx);
                if (!targets.empty()) {
                    nextTarget = targets[0]; 
                } else {
                    dir = STOPPED;
                }
            }

            if (nextTarget != -1) {
                if (currentFloor < nextTarget) {
                    dir = UP;
                    this_thread::sleep_for(seconds(1));
                    currentFloor++;
                    printStatus();
                } else if (currentFloor > nextTarget) {
                    dir = DOWN;
                    this_thread::sleep_for(seconds(1));
                    currentFloor--;
                    printStatus();
                }

                // РџСЂРѕРІРµСЂСЏРµРј РµС‰Рµ СЂР°Р·, РЅРµ РѕС‡РёСЃС‚РёР»Рё Р»Рё С†РµР»Рё (РЅР° СЃР»СѓС‡Р°Р№ РЅР°Р¶Р°С‚РёСЏ S РІРѕ РІСЂРµРјСЏ sleep)
                if (currentFloor == nextTarget) {
                    lock_guard<mutex> lock(mtx);
                    if (!targets.empty() && targets[0] == currentFloor) {
                        cout << "Cabin on floor " << currentFloor << ". Stopped. Doors opened." << endl;
                        targets.erase(targets.begin());
                    }
                }
            } else {
                this_thread::sleep_for(milliseconds(100));
            }
        }
    }

    void printStatus() {
        string dirStr = (dir == UP) ? "Going up" : (dir == DOWN) ? "Going down" : "Stopped";
        cout << "Cabin on floor " << currentFloor << ". " << dirStr 
             << ". <Press any key to enter new command>" << endl;
    }

    void stop() { running = false; }
};

int main() {
    int floors;
    cout << "Enter number of floors: ";
    if (!(cin >> floors)) return 1;

    Elevator elevator(floors);
    thread elevatorThread(&Elevator::run, &elevator);

    string input;
    cout << "System ready. Commands: F[n], C[n], S (Stop) or 'exit'" << endl;
    
    while (true) {
        if (!(cin >> input)) break;
        if (input == "exit") break;

        try {
            char type = toupper(input[0]);
            
            // РћР±СЂР°Р±РѕС‚РєР° РєРЅРѕРїРєРё S
            if (type == 'S') {
                elevator.emergencyStop();
                continue;
            }

            if (input.length() > 1) {
                int targetFloor = stoi(input.substr(1));
                if (type == 'F' || type == 'C') {
                    elevator.addTarget(targetFloor);
                }
            }
        } catch (...) {
            cout << "Invalid command format. Use F5, C2, S, etc." << endl;
        }
    }

    elevator.stop();
    if (elevatorThread.joinable()) elevatorThread.join();
    return 0;
}

