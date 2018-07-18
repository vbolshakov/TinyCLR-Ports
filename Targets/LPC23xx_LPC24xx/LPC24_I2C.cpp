// Copyright Microsoft Corporation
// Copyright GHI Electronics, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "LPC24.h"

#define I2C_TRANSACTION_TIMEOUT 2000 // 2 seconds
#define TOTAL_I2C_CONTROLLERS SIZEOF_ARRAY(i2cSclPins)

struct I2cConfiguration {

    int32_t                  address;
    uint8_t                  clockRate;     // primary clock factor to generate the i2c clock
    uint8_t                  clockRate2;   // additional clock factors, if more than one is needed for the clock (optional)

    bool                     isOpened;
};

struct I2cTransaction {
    bool                        isReadTransaction;
    bool                        repeatedStart;
    bool                        isDone;

    uint8_t                     *buffer;

    size_t                      bytesToTransfer;
    size_t                      bytesTransferred;

    TinyCLR_I2c_TransferStatus  result;
};

static const LPC24_Gpio_Pin i2cSclPins[] = LPC24_I2C_SCL_PINS;
static const LPC24_Gpio_Pin i2cSdaPins[] = LPC24_I2C_SDA_PINS;

struct I2cDriver {
    int32_t controllerIndex;

    I2cConfiguration i2cConfiguration;
    I2cTransaction   *currentI2cTransactionAction;
    I2cTransaction   readI2cTransactionAction;
    I2cTransaction   writeI2cTransactionAction;
};

static I2cDriver i2cDrivers[TOTAL_I2C_CONTROLLERS];

static TinyCLR_I2c_Controller i2cControllers[TOTAL_I2C_CONTROLLERS];
static TinyCLR_Api_Info i2cApi[TOTAL_I2C_CONTROLLERS];

const TinyCLR_Api_Info* LPC24_I2c_GetApi() {
    for (auto i = 0; i < TOTAL_I2C_CONTROLLERS; i++) {
        i2cControllers[i].ApiInfo = &i2cApi[i];
        i2cControllers[i].Acquire = &LPC24_I2c_Acquire;
        i2cControllers[i].Release = &LPC24_I2c_Release;
        i2cControllers[i].SetActiveSettings = &LPC24_I2c_SetActiveSettings;
        i2cControllers[i].Read = &LPC24_I2c_Read;
        i2cControllers[i].Write = &LPC24_I2c_Write;
        i2cControllers[i].WriteRead = &LPC24_I2c_WriteRead;

        i2cApi[i].Author = "GHI Electronics, LLC";
        i2cApi[i].Name = "GHIElectronics.TinyCLR.NativeApis.LPC24.I2cController";
        i2cApi[i].Type = TinyCLR_Api_Type::I2cController;
        i2cApi[i].Version = 0;
        i2cApi[i].Implementation = &i2cControllers[i];
        i2cApi[i].State = &i2cDrivers[i];

        i2cDrivers[i].controllerIndex = i;
    }

    return (const TinyCLR_Api_Info*)&i2cApi;
}

void LPC24_I2c_InterruptHandler(void *param) {
    uint8_t address;

    int32_t controllerIndex = *reinterpret_cast<int32_t*>(param);

    auto driver = &i2cDrivers[controllerIndex];

    LPC24XX_I2C& I2C = LPC24XX::I2C(controllerIndex);

    // read status
    uint8_t status = I2C.I2STAT;

    I2cTransaction *transaction = driver->currentI2cTransactionAction;

    if (!transaction) {
        I2C.I2CONCLR = LPC24XX_I2C::SI;
        return;
    }

    switch (status) {
    case 0x08: // Start Condition transmitted
    case 0x10: // Repeated Start Condition transmitted
        // Write Slave address and Data direction
        address = 0xFE & (driver->i2cConfiguration.address << 1);
        address |= transaction->isReadTransaction ? 1 : 0;
        I2C.I2DAT = address;
        // Clear STA bit
        I2C.I2CONCLR = LPC24XX_I2C::STA;
        break;
    case 0x18: // Slave Address + W transmitted, Ack received
    case 0x28: // Data transmitted, Ack received
        // Write data
        // transaction completed
        if (transaction->bytesToTransfer == 0) {
            if (transaction->repeatedStart == false) {
                LPC24_I2c_StopTransaction(controllerIndex);
            }
            else {
                driver->currentI2cTransactionAction = &driver->readI2cTransactionAction;
                LPC24_I2c_StartTransaction(controllerIndex);
            }
        }
        else {
            //WriteToSlave( unit );
            I2C.I2DAT = transaction->buffer[transaction->bytesTransferred];

            transaction->bytesTransferred++;
            transaction->bytesToTransfer--;

        }
        break;
    case 0x20: // Write Address not acknowledged by slave
    case 0x30: // Data not acknowledged by slave
    case 0x48: // Read Address not acknowledged by slave
        LPC24_I2c_StopTransaction(controllerIndex);
        break;
    case 0x38: // Arbitration lost
        LPC24_I2c_StopTransaction(controllerIndex);
        break;
    case 0x40: // Slave Address + R transmitted, Ack received
        // if the transaction is one byte only to read, then we must send NAK immediately
        if (transaction->bytesToTransfer == 1) {
            I2C.I2CONCLR = LPC24XX_I2C::AA;
        }
        else {
            I2C.I2CONSET = LPC24XX_I2C::AA;
        }
        break;
    case 0x50: // Data received, Ack Sent
    case 0x58: // Data received, NO Ack sent
        // read next byte
        //ReadFromSlave( unit );
        transaction->buffer[transaction->bytesTransferred] = I2C.I2DAT;

        transaction->bytesTransferred++;
        transaction->bytesToTransfer--;

        if (transaction->bytesToTransfer == 1) {
            I2C.I2CONCLR = LPC24XX_I2C::AA;
        }
        if (transaction->bytesToTransfer == 0) {
            if (transaction->repeatedStart == false) {
                // send transaction stop
                LPC24_I2c_StopTransaction(controllerIndex);
            }
            else {
                // start next
                driver->currentI2cTransactionAction = &driver->readI2cTransactionAction;
                LPC24_I2c_StartTransaction(controllerIndex);
            }
        }
        break;
    case 0x00: // Bus Error
        // Clear Bus error
        I2C.I2CONSET = LPC24XX_I2C::STO;
        LPC24_I2c_StopTransaction(controllerIndex);
        break;
    default:
        LPC24_I2c_StopTransaction(controllerIndex);
        break;
    } // switch(status)

    // clear the interrupt flag to start the next I2C transfer
    I2C.I2CONCLR = LPC24XX_I2C::SI;
}
void LPC24_I2c_StartTransaction(int32_t controllerIndex) {
    LPC24XX_I2C& I2C = LPC24XX::I2C(controllerIndex);

    auto driver = &i2cDrivers[controllerIndex];

    if (!driver->writeI2cTransactionAction.repeatedStart || driver->writeI2cTransactionAction.bytesTransferred == 0) {
        I2C.I2SCLH = driver->i2cConfiguration.clockRate | (driver->i2cConfiguration.clockRate2 << 8);
        I2C.I2SCLL = driver->i2cConfiguration.clockRate | (driver->i2cConfiguration.clockRate2 << 8);

        I2C.I2CONSET = LPC24XX_I2C::STA;
    }
    else {
        I2C.I2CONSET = LPC24XX_I2C::STA;
    }

}

void LPC24_I2c_StopTransaction(int32_t controllerIndex) {
    LPC24XX_I2C& I2C = LPC24XX::I2C(controllerIndex);

    I2C.I2CONSET = LPC24XX_I2C::STO;
    I2C.I2CONCLR = LPC24XX_I2C::AA | LPC24XX_I2C::SI | LPC24XX_I2C::STA;

    auto driver = &i2cDrivers[controllerIndex];
    driver->currentI2cTransactionAction->isDone = true;
}

TinyCLR_Result LPC24_I2c_Read(const TinyCLR_I2c_Controller* self, uint8_t* buffer, size_t& length, TinyCLR_I2c_TransferStatus& result) {
    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    int32_t timeout = I2C_TRANSACTION_TIMEOUT;

    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);
    auto controllerIndex = driver->controllerIndex;

    driver->readI2cTransactionAction.isReadTransaction = true;
    driver->readI2cTransactionAction.buffer = buffer;
    driver->readI2cTransactionAction.bytesToTransfer = length;
    driver->readI2cTransactionAction.isDone = false;
    driver->readI2cTransactionAction.repeatedStart = false;
    driver->readI2cTransactionAction.bytesTransferred = 0;

    driver->currentI2cTransactionAction = &driver->readI2cTransactionAction;

    LPC24_I2c_StartTransaction(controllerIndex);

    while (driver->currentI2cTransactionAction->isDone == false && timeout > 0) {
        LPC24_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (driver->currentI2cTransactionAction->bytesTransferred == length)
        result = TinyCLR_I2c_TransferStatus::FullTransfer;
    else if (driver->currentI2cTransactionAction->bytesTransferred < length && driver->currentI2cTransactionAction->bytesTransferred > 0)
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;

    length = driver->currentI2cTransactionAction->bytesTransferred;

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result LPC24_I2c_Write(const TinyCLR_I2c_Controller* self, const uint8_t* buffer, size_t& length, TinyCLR_I2c_TransferStatus& result) {
    int32_t timeout = I2C_TRANSACTION_TIMEOUT;

    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);
    auto controllerIndex = driver->controllerIndex;

    driver->writeI2cTransactionAction.isReadTransaction = false;
    driver->writeI2cTransactionAction.buffer = (uint8_t*)buffer;
    driver->writeI2cTransactionAction.bytesToTransfer = length;
    driver->writeI2cTransactionAction.isDone = false;
    driver->writeI2cTransactionAction.repeatedStart = false;
    driver->writeI2cTransactionAction.bytesTransferred = 0;

    driver->currentI2cTransactionAction = &driver->writeI2cTransactionAction;

    LPC24_I2c_StartTransaction(controllerIndex);

    while (driver->currentI2cTransactionAction->isDone == false && timeout > 0) {
        LPC24_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (driver->currentI2cTransactionAction->bytesTransferred == length)
        result = TinyCLR_I2c_TransferStatus::FullTransfer;
    else if (driver->currentI2cTransactionAction->bytesTransferred < length && driver->currentI2cTransactionAction->bytesTransferred > 0)
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;

    length = driver->currentI2cTransactionAction->bytesTransferred;

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result LPC24_I2c_WriteRead(const TinyCLR_I2c_Controller* self, const uint8_t* writeBuffer, size_t& writeLength, uint8_t* readBuffer, size_t& readLength, TinyCLR_I2c_TransferStatus& result) {
    int32_t timeout = I2C_TRANSACTION_TIMEOUT;

    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);
    auto controllerIndex = driver->controllerIndex;

    driver->writeI2cTransactionAction.isReadTransaction = false;
    driver->writeI2cTransactionAction.buffer = (uint8_t*)writeBuffer;
    driver->writeI2cTransactionAction.bytesToTransfer = writeLength;
    driver->writeI2cTransactionAction.isDone = false;
    driver->writeI2cTransactionAction.repeatedStart = true;
    driver->writeI2cTransactionAction.bytesTransferred = 0;

    driver->readI2cTransactionAction.isReadTransaction = true;
    driver->readI2cTransactionAction.buffer = readBuffer;
    driver->readI2cTransactionAction.bytesToTransfer = readLength;
    driver->readI2cTransactionAction.isDone = false;
    driver->readI2cTransactionAction.repeatedStart = false;
    driver->readI2cTransactionAction.bytesTransferred = 0;

    driver->currentI2cTransactionAction = &driver->writeI2cTransactionAction;

    LPC24_I2c_StartTransaction(controllerIndex);

    while (driver->currentI2cTransactionAction->isDone == false && timeout > 0) {
        LPC24_Time_Delay(nullptr, 1000);

        timeout--;
    }

    if (driver->writeI2cTransactionAction.bytesTransferred != writeLength) {
        writeLength = driver->writeI2cTransactionAction.bytesTransferred;
        result = TinyCLR_I2c_TransferStatus::PartialTransfer;
    }
    else {
        readLength = driver->readI2cTransactionAction.bytesTransferred;

        if (driver->currentI2cTransactionAction->bytesTransferred == readLength)
            result = TinyCLR_I2c_TransferStatus::FullTransfer;
        else if (driver->currentI2cTransactionAction->bytesTransferred < readLength && driver->currentI2cTransactionAction->bytesTransferred > 0)
            result = TinyCLR_I2c_TransferStatus::PartialTransfer;
    }

    return timeout > 0 ? TinyCLR_Result::Success : TinyCLR_Result::TimedOut;
}

TinyCLR_Result LPC24_I2c_SetActiveSettings(const TinyCLR_I2c_Controller* self, int32_t slaveAddress, TinyCLR_I2c_BusSpeed busSpeed) {
    uint32_t rateKhz;

    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    if (busSpeed == TinyCLR_I2c_BusSpeed::FastMode)
        rateKhz = 400; // FastMode
    else if (busSpeed == TinyCLR_I2c_BusSpeed::StandardMode)
        rateKhz = 100; // StandardMode
    else
        return TinyCLR_Result::NotSupported;

    uint32_t divider = LPC24XX_I2C::c_I2C_Clk_KHz / (2 * rateKhz);
    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);

    driver->i2cConfiguration.clockRate = (uint8_t)divider; // low byte
    driver->i2cConfiguration.clockRate2 = (uint8_t)(divider >> 8); // high byte
    driver->i2cConfiguration.address = slaveAddress;

    return TinyCLR_Result::Success;
}

TinyCLR_Result LPC24_I2c_Acquire(const TinyCLR_I2c_Controller* self) {
    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);
    auto controllerIndex = driver->controllerIndex;

    LPC24XX_I2C& I2C = LPC24XX::I2C(controllerIndex);

    if (!LPC24_Gpio_OpenPin(i2cSdaPins[controllerIndex].number) || !LPC24_Gpio_OpenPin(i2cSclPins[controllerIndex].number))
        return TinyCLR_Result::SharingViolation;

    LPC24_Gpio_ConfigurePin(i2cSdaPins[controllerIndex].number, LPC24_Gpio_Direction::Input, i2cSdaPins[controllerIndex].pinFunction, LPC24_Gpio_PinMode::Inactive);
    LPC24_Gpio_ConfigurePin(i2cSclPins[controllerIndex].number, LPC24_Gpio_Direction::Input, i2cSclPins[controllerIndex].pinFunction, LPC24_Gpio_PinMode::Inactive);

    LPC24_Interrupt_Activate(controllerIndex == 0 ? LPC24XX_VIC::c_IRQ_INDEX_I2C0 : (controllerIndex == 1 ? LPC24XX_VIC::c_IRQ_INDEX_I2C1 : LPC24XX_VIC::c_IRQ_INDEX_I2C2), (uint32_t*)&LPC24_I2c_InterruptHandler, (uint32_t*)&driver->controllerIndex);

    // enable the I2c module
    I2C.I2CONSET = LPC24XX_I2C::I2EN;

    // set the slave address
    I2C.I2ADR = 0x7E;

    driver->i2cConfiguration.isOpened = true;

    return TinyCLR_Result::Success;
}

TinyCLR_Result LPC24_I2c_Release(const TinyCLR_I2c_Controller* self) {
    if (self == nullptr)
        return TinyCLR_Result::ArgumentNull;

    auto driver = reinterpret_cast<I2cDriver*>(self->ApiInfo->State);
    auto controllerIndex = driver->controllerIndex;

    LPC24XX_I2C& I2C = LPC24XX::I2C(controllerIndex);

    LPC24_Interrupt_Deactivate(controllerIndex == 0 ? LPC24XX_VIC::c_IRQ_INDEX_I2C0 : (controllerIndex == 1 ? LPC24XX_VIC::c_IRQ_INDEX_I2C1 : LPC24XX_VIC::c_IRQ_INDEX_I2C2));

    I2C.I2CONCLR = (LPC24XX_I2C::AA | LPC24XX_I2C::SI | LPC24XX_I2C::STO | LPC24XX_I2C::STA | LPC24XX_I2C::I2EN);

    if (driver->i2cConfiguration.isOpened) {
        LPC24_Gpio_ClosePin(i2cSclPins[controllerIndex].number);
        LPC24_Gpio_ClosePin(i2cSdaPins[controllerIndex].number);
    }

    driver->i2cConfiguration.isOpened = false;

    return TinyCLR_Result::Success;
}

void LPC24_I2c_Reset() {
    for (auto i = 0; i < TOTAL_I2C_CONTROLLERS; i++) {
        LPC24_I2c_Release(&i2cControllers[i]);

        auto driver = &i2cDrivers[i];

        driver->i2cConfiguration.address = 0;
        driver->i2cConfiguration.clockRate = 0;
        driver->i2cConfiguration.clockRate2 = 0;

        driver->readI2cTransactionAction.bytesToTransfer = 0;
        driver->readI2cTransactionAction.bytesTransferred = 0;

        driver->writeI2cTransactionAction.bytesToTransfer = 0;
        driver->writeI2cTransactionAction.bytesTransferred = 0;

        driver->i2cConfiguration.isOpened = false;
    }
}

