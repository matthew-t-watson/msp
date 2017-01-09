#ifndef FLIGHTCONTROLLER_HPP
#define FLIGHTCONTROLLER_HPP

#include "MSP.hpp"
#include "msp_msg.hpp"

#include "PeriodicTimer.hpp"
#include <map>

namespace fcu {

typedef unsigned int uint;

enum class FirmwareType {
    MULTIWII,
    CLEANFLIGHT
};

class SubscriptionBase {
public:
    SubscriptionBase(PeriodicTimer *timer=NULL) : timer(timer) { }

    virtual ~SubscriptionBase() {
        if(timer!=NULL) { delete timer; }
    }

    virtual void call(const msp::Request &req) = 0;

    bool hasTimer() {
        return timer!=NULL;
    }

protected:
    PeriodicTimer *timer;
};

template<typename T, typename C>
class Subscription : public SubscriptionBase {
public:
    typedef void(C::*Callback)(const T&);

    Subscription(const Callback caller, C *const context_class)
        : funct(caller), context(context_class) {
    }

    Subscription(const Callback caller, C *const context_class, PeriodicTimer *timer)
        : SubscriptionBase(timer), funct(caller), context(context_class)
    {
        this->timer->start();
    }

    void call(const msp::Request &req) {
        (*context.*funct)( dynamic_cast<const T&>(req) );
    }

private:
    Callback funct;
    C *const context;
};

class FlightController {
public:
    FlightController(const std::string &device, const uint baudrate=115200);

    ~FlightController();

    /**
     * @brief getMSP expose underlying MSP instance for low-level access
     * @return reference to MSP instance
     */
    msp::MSP &getMSP() { return msp; }

    void waitForConnection();

    void initialise();

    /**
     * @brief isFirmware determine firmware type (e.g. to distiguish accepted messages)
     * @param firmware_type type of firmware (enum FirmwareType)
     * @return true if firmware is firmware_type
     * @return false if firmware is not firmware_type
     */
    bool isFirmware(const FirmwareType firmware_type);

    bool isFirmwareMultiWii() { return isFirmware(FirmwareType::MULTIWII); }

    bool isFirmwareCleanflight() { return isFirmware(FirmwareType::CLEANFLIGHT); }

    template<typename T>
    /**
     * @brief registerMessage register a message and its ID
     * @param id ID of message
     */
    void registerMessage(const msp::ID &id) {
        // delete old message
        if(database.count(id)==1)
            delete database[id];

        // create message
        switch(id) {
        case msp::ID::MSP_RAW_IMU:
            database[id] = new msp::Imu(acc_1g, gyro_unit, magn_gain, standard_gravity);
            break;
        default:
            // use default constructor
            database[id] = new T();
            break;
        }
    }

    /**
     * @brief subscribe register callback function that is called when type is received
     * @param callback pointer to callback function (method of class)
     * @param context class of callback method
     */
    template<typename T, typename C>
    void subscribe(void (C::*callback)(const T&), C *context, const double tp = 0.0) {
        const msp::ID id = T().id();
        registerMessage<T>(id);
        if(std::is_base_of<msp::Request, T>::value) {
            if(tp>0.0) {
                // subscription with periodic sending of requests
                subscriptions[id] = new Subscription<T,C>(callback, context,
                    new PeriodicTimer(
                        std::bind(&FlightController::sendRequest, this, T().id()),
                        tp
                    )
                );
            }
            else {
                // subscription with manual sending of requests
                subscriptions[T().id()] = new Subscription<T,C>(callback, context);
            }
        }
        else {
            throw std::runtime_error("Callback parameter needs to be of Request type!");
        }
    }

    /**
     * @brief handle listen for messages and call callback functions
     */
    void handle() {
        sendRequests();
        handleRequests();
    }

    /**
     * @brief sendRequests send all subscribed requests
     */
    void sendRequests();

    /**
     * @brief sendRequest send request with ID
     * @param id message ID of request
     * @return true on success
     * @return false on failure
     */
    bool sendRequest(const msp::ID id);

    /**
     * @brief handleRequests read incomming data and call corresponding callbacks
     */
    void handleRequests();

    void setAcc1G(const float acc1g) { acc_1g=acc1g; }

    void setGyroUnit(const float gyro) { gyro_unit=gyro; }

    void setMagnGain(const float gain) { magn_gain=gain; }

    void setStandardGravity(const float gravity) { standard_gravity=gravity; }

    void initBoxes();

    bool hasCapability(const msp::Capability &cap) const {
        return ident.capabilities.count(cap);
    }

    bool hasBind() const {
        return hasCapability(msp::Capability::BIND);
    }

    bool hasDynBal() const {
        return hasCapability(msp::Capability::DYNBAL);
    }

    bool hasFlap() const {
        return hasCapability(msp::Capability::FLAP);
    }

    bool hasSensor(const msp::Sensor &sensor) const {
        return sensors.count(sensor);
    }

    bool hasAccelerometer() const {
        return hasSensor(msp::Sensor::Accelerometer);
    }

    bool hasBarometer() const {
        return hasSensor(msp::Sensor::Barometer);
    }

    bool hasMagnetometer() const {
        return hasSensor(msp::Sensor::Magnetometer);
    }

    bool hasGPS() const {
        return hasSensor(msp::Sensor::GPS);
    }

    bool hasSonar() const {
        return hasSensor(msp::Sensor::Sonar);
    }

    bool isArmed();

    bool setRc(const uint16_t roll, const uint16_t pitch,
               const uint16_t yaw, const uint16_t throttle,
               const uint16_t aux1 = 1000, const uint16_t aux2 = 1000,
               const uint16_t aux3 = 1000, const uint16_t aux4 = 1000);

    bool setMotors(const std::array<uint16_t,msp::N_MOTOR> &motor_values);

    /**
     * @brief arm arm or disarm FC
     * @param arm true: will arm FC, false: will disarm FC
     * @return true on success
     */
    bool arm(const bool arm);

    /**
     * @brief arm_block attempt to arm and wait for status feedback, e.g. this method will block until the FC is able to aim
     * @return
     */
    bool arm_block();

    /**
     * @brief disarm_block attempt to disarm and wait for status feedback
     * @return
     */
    bool disarm_block();

private:
    msp::Request* getRequestById(const msp::ID id) {
        return database[id];
    }

    msp::MSP msp;

    std::map<msp::ID, msp::Request*> database;

    std::map<msp::ID, SubscriptionBase*> subscriptions;

   // sensor specific units
    float acc_1g;       // reported acceleration value at 1G

    float gyro_unit;

    float magn_gain;    // scale magnetic value to uT (micro Tesla)

    float standard_gravity; // standard gravity for 1g in m/s^2

    std::map<std::string, uint> box_name_ids;

    msp::Ident ident;

    std::set<msp::Sensor> sensors;

    FirmwareType firmware;
};

} // namespace msp

#endif // FLIGHTCONTROLLER_HPP
