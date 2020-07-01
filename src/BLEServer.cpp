/*
 * BLEServer.cpp
 *
 *  Created on: Apr 16, 2017
 *      Author: kolban
 */

#include "sdkconfig.h"
#if defined(CONFIG_BT_ENABLED)
#include <esp_bt.h>
#include <esp_bt_main.h>
#include "GeneralUtils.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEService.h"
#include "BLEUtils.h"
#include <string.h>
#include <string>
#include <unordered_set>
#include "logger.h"
#define LOG_TAG "BLEServer"

static Logger& logger = Logger::instance();

/**
 * @brief Construct a %BLE Server
 *
 * This class is not designed to be individually instantiated.  Instead one should create a server by asking
 * the BLEDevice class.
 */
BLEServer::BLEServer() {
	m_appId            = ESP_GATT_IF_NONE;
	m_gatts_if         = ESP_GATT_IF_NONE;
	m_connectedCount   = 0;
	m_connId           = ESP_GATT_IF_NONE;
	m_pServerCallbacks = nullptr;
} // BLEServer


void BLEServer::createApp(uint16_t appId) {
	m_appId = appId;
	registerApp(appId);
} // createApp


/**
 * @brief Create a %BLE Service.
 *
 * With a %BLE server, we can host one or more services.  Invoking this function causes the creation of a definition
 * of a new service.  Every service must have a unique UUID.
 * @param [in] uuid The UUID of the new service.
 * @return A reference to the new service object.
 */
BLEService* BLEServer::createService(const char* uuid) {
	return createService(BLEUUID(uuid));
}


/**
 * @brief Create a %BLE Service.
 *
 * With a %BLE server, we can host one or more services.  Invoking this function causes the creation of a definition
 * of a new service.  Every service must have a unique UUID.
 * @param [in] uuid The UUID of the new service.
 * @param [in] numHandles The maximum number of handles associated with this service.
 * @param [in] inst_id With multiple services with the same UUID we need to provide inst_id value different for each service.
 * @return A reference to the new service object.
 */
BLEService* BLEServer::createService(BLEUUID uuid, uint32_t numHandles, uint8_t inst_id) {
	logger.debug(LOG_TAG, String(">> createService - ") + uuid.toString().c_str());
	m_semaphoreCreateEvt.take("createService");

	// Check that a service with the supplied UUID does not already exist.
	if (m_serviceMap.getByUUID(uuid) != nullptr) {
		logger.warning(LOG_TAG, String("<< Attempt to create a new service with uuid ") + uuid.toString().c_str() +
		                               "but a service with that UUID already exists.");
	}

	BLEService* pService = new BLEService(uuid, numHandles);
	pService->m_instId = inst_id;
	m_serviceMap.setByUUID(uuid, pService); // Save a reference to this service being on this server.
	pService->executeCreate(this);          // Perform the API calls to actually create the service.

	m_semaphoreCreateEvt.wait("createService");

	logger.debug(LOG_TAG, "<< createService");
	return pService;
} // createService


/**
 * @brief Get a %BLE Service by its UUID
 * @param [in] uuid The UUID of the new service.
 * @return A reference to the service object.
 */
BLEService* BLEServer::getServiceByUUID(const char* uuid) {
	return m_serviceMap.getByUUID(uuid);
}

/**
 * @brief Get a %BLE Service by its UUID
 * @param [in] uuid The UUID of the new service.
 * @return A reference to the service object.
 */
BLEService* BLEServer::getServiceByUUID(BLEUUID uuid) {
	return m_serviceMap.getByUUID(uuid);
}

/**
 * @brief Retrieve the advertising object that can be used to advertise the existence of the server.
 *
 * @return An advertising object.
 */
BLEAdvertising* BLEServer::getAdvertising() {
	return BLEDevice::getAdvertising();
}

uint16_t BLEServer::getConnId() {
	return m_connId;
}


/**
 * @brief Return the number of connected clients.
 * @return The number of connected clients.
 */
uint32_t BLEServer::getConnectedCount() {
	return m_connectedCount;
} // getConnectedCount


uint16_t BLEServer::getGattsIf() {
	return m_gatts_if;
}


/**
 * @brief Handle a GATT Server Event.
 *
 * @param [in] event
 * @param [in] gatts_if
 * @param [in] param
 *
 */
void BLEServer::handleGATTServerEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
	logger.debug(LOG_TAG, String(">> handleGATTServerEvent: ") +
		BLEUtils::gattServerEventTypeToString(event).c_str());

	switch(event) {
		// ESP_GATTS_ADD_CHAR_EVT - Indicate that a characteristic was added to the service.
		// add_char:
		// - esp_gatt_status_t status
		// - uint16_t          attr_handle
		// - uint16_t          service_handle
		// - esp_bt_uuid_t     char_uuid
		//
		case ESP_GATTS_ADD_CHAR_EVT: {
			break;
		} // ESP_GATTS_ADD_CHAR_EVT

		case ESP_GATTS_MTU_EVT:
			updatePeerMTU(param->mtu.conn_id, param->mtu.mtu);
			break;

		// ESP_GATTS_CONNECT_EVT
		// connect:
		// - uint16_t      conn_id
		// - esp_bd_addr_t remote_bda
		//
		case ESP_GATTS_CONNECT_EVT: {
			m_connId = param->connect.conn_id;
			addPeerDevice((void*)this, false, m_connId);
			memcpy(&m_client_addr, param->connect.remote_bda, 6);
			if (m_pServerCallbacks != nullptr) {
				m_pServerCallbacks->onConnect(this);
				m_pServerCallbacks->onConnect(this, param);			
			}
			m_connectedCount++;   // Increment the number of connected devices count.	
			break;
		} // ESP_GATTS_CONNECT_EVT


		// ESP_GATTS_CREATE_EVT
		// Called when a new service is registered as having been created.
		//
		// create:
		// * esp_gatt_status_t  status
		// * uint16_t           service_handle
		// * esp_gatt_srvc_id_t service_id
		//
		case ESP_GATTS_CREATE_EVT: {
			BLEService* pService = m_serviceMap.getByUUID(param->create.service_id.id.uuid, param->create.service_id.id.inst_id);  // <--- very big bug for multi services with the same uuid
			m_serviceMap.setByHandle(param->create.service_handle, pService);
			m_semaphoreCreateEvt.give();
			break;
		} // ESP_GATTS_CREATE_EVT


		// ESP_GATTS_DISCONNECT_EVT
		//
		// disconnect
		// - uint16_t      					conn_id
		// - esp_bd_addr_t 					remote_bda
		// - esp_gatt_conn_reason_t         reason
		//
		// If we receive a disconnect event then invoke the callback for disconnects (if one is present).
		// we also want to start advertising again.
		case ESP_GATTS_DISCONNECT_EVT: {
			m_connectedCount--;                          // Decrement the number of connected devices count.
			if (m_pServerCallbacks != nullptr) {         // If we have callbacks, call now.
				m_pServerCallbacks->onDisconnect(this);
			}
			startAdvertising(); //- do this with some delay from the loop()
			removePeerDevice(param->disconnect.conn_id, false);
			break;
		} // ESP_GATTS_DISCONNECT_EVT


		// ESP_GATTS_READ_EVT - A request to read the value of a characteristic has arrived.
		//
		// read:
		// - uint16_t      conn_id
		// - uint32_t      trans_id
		// - esp_bd_addr_t bda
		// - uint16_t      handle
		// - uint16_t      offset
		// - bool          is_long
		// - bool          need_rsp
		//
		case ESP_GATTS_READ_EVT: {
			break;
		} // ESP_GATTS_READ_EVT


		// ESP_GATTS_REG_EVT
		// reg:
		// - esp_gatt_status_t status
		// - uint16_t app_id
		//
		case ESP_GATTS_REG_EVT: {
			m_gatts_if = gatts_if;
			m_semaphoreRegisterAppEvt.give(); // Unlock the mutex waiting for the registration of the app.
			break;
		} // ESP_GATTS_REG_EVT


		// ESP_GATTS_WRITE_EVT - A request to write the value of a characteristic has arrived.
		//
		// write:
		// - uint16_t      conn_id
		// - uint16_t      trans_id
		// - esp_bd_addr_t bda
		// - uint16_t      handle
		// - uint16_t      offset
		// - bool          need_rsp
		// - bool          is_prep
		// - uint16_t      len
		// - uint8_t*      value
		//
		case ESP_GATTS_WRITE_EVT: {
			break;
		}

		case ESP_GATTS_OPEN_EVT:
			m_semaphoreOpenEvt.give(param->open.status);
			break;

		default:
			break;
	}

	// Invoke the handler for every Service we have.
	m_serviceMap.handleGATTServerEvent(event, gatts_if, param);

	logger.debug(LOG_TAG, "<< handleGATTServerEvent");
} // handleGATTServerEvent


/**
 * @brief Register the app.
 *
 * @return N/A
 */
void BLEServer::registerApp(uint16_t m_appId) {
	logger.debug(LOG_TAG, ">> registerApp - " + String(m_appId));
	m_semaphoreRegisterAppEvt.take("registerApp"); // Take the mutex, will be released by ESP_GATTS_REG_EVT event.
	::esp_ble_gatts_app_register(m_appId);
	m_semaphoreRegisterAppEvt.wait("registerApp");
	logger.debug(LOG_TAG, "<< registerApp");
} // registerApp


/**
 * @brief Set the server callbacks.
 *
 * As a %BLE server operates, it will generate server level events such as a new client connecting or a previous client
 * disconnecting.  This function can be called to register a callback handler that will be invoked when these
 * events are detected.
 *
 * @param [in] pCallbacks The callbacks to be invoked.
 */
void BLEServer::setCallbacks(BLEServerCallbacks* pCallbacks) {
	m_pServerCallbacks = pCallbacks;
} // setCallbacks

/*
 * Remove service
 */
void BLEServer::removeService(BLEService* service) {
	service->stop();
	service->executeDelete();	
	m_serviceMap.removeService(service);
}

/**
 * @brief Start advertising.
 *
 * Start the server advertising its existence.  This is a convenience function and is equivalent to
 * retrieving the advertising object and invoking start upon it.
 */
void BLEServer::startAdvertising() {
	logger.debug(LOG_TAG, ">> startAdvertising");
	BLEDevice::startAdvertising();
	logger.debug(LOG_TAG, "<< startAdvertising");
} // startAdvertising

/**
 * Allow to connect GATT server to peer device
 * Probably can be used in ANCS for iPhone
 */
bool BLEServer::connect(BLEAddress address) {
	logger.debug(LOG_TAG,">> connect()");
	esp_bd_addr_t addr;
	memcpy(&addr, address.getNative(), 6);
	// Perform the open connection request against the target BLE Server.
	m_semaphoreOpenEvt.take("connect");
	esp_err_t errRc = ::esp_ble_gatts_open(
		getGattsIf(),
		addr, // address
		1     // direct connection
	);
	if (errRc != ESP_OK) {
		logger.error(LOG_TAG, String("esp_ble_gattc_open: rc=") + String(errRc) + " " + GeneralUtils::errorToString(errRc));
		return false;
	}

	uint32_t rc = m_semaphoreOpenEvt.wait("connect");   // Wait for the connection to complete.
	logger.debug(LOG_TAG, String("<< connect(), rc=") + (rc==ESP_GATT_OK));
	return rc == ESP_GATT_OK;
} // connect



void BLEServerCallbacks::onConnect(BLEServer* pServer) {
	logger.debug("BLEServerCallbacks", ">> onConnect(): Default");
	logger.debug("BLEServerCallbacks", String("Device: ") + BLEDevice::toString().c_str());
	logger.debug("BLEServerCallbacks", "<< onConnect()");
} // onConnect

void BLEServerCallbacks::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
	logger.debug("BLEServerCallbacks", ">> onConnect(): Default");
	logger.debug("BLEServerCallbacks", String("Device: ") + BLEDevice::toString().c_str());
	logger.debug("BLEServerCallbacks", "<< onConnect()");
} // onConnect


void BLEServerCallbacks::onDisconnect(BLEServer* pServer) {
	logger.debug("BLEServerCallbacks", ">> onDisconnect(): Default");
	logger.debug("BLEServerCallbacks", String("Device: ") + BLEDevice::toString().c_str());
	logger.debug("BLEServerCallbacks", "<< onDisconnect()");
} // onDisconnect

/* multi connect support */
/* TODO do some more tweaks */
void BLEServer::updatePeerMTU(uint16_t conn_id, uint16_t mtu) {
	// set mtu in conn_status_t
	const std::map<uint16_t, conn_status_t>::iterator it = m_connectedServersMap.find(conn_id);
	if (it != m_connectedServersMap.end()) {
		it->second.mtu = mtu;
		std::swap(m_connectedServersMap[conn_id], it->second);
	}
}

std::map<uint16_t, conn_status_t> BLEServer::getPeerDevices(bool _client) {
	return m_connectedServersMap;
}


uint16_t BLEServer::getPeerMTU(uint16_t conn_id) {
	return m_connectedServersMap.find(conn_id)->second.mtu;
}

void BLEServer::addPeerDevice(void* peer, bool _client, uint16_t conn_id) {
	conn_status_t status = {
		.peer_device = peer,
		.connected = true,
		.mtu = 23
	};

	m_connectedServersMap.insert(std::pair<uint16_t, conn_status_t>(conn_id, status));	
}

void BLEServer::removePeerDevice(uint16_t conn_id, bool _client) {
	m_connectedServersMap.erase(conn_id);
}
/* multi connect support */

/**
 * Update connection parameters can be called only after connection has been established
 */
void BLEServer::updateConnParams(esp_bd_addr_t remote_bda, uint16_t minInterval, uint16_t maxInterval, uint16_t latency, uint16_t timeout) {
	esp_ble_conn_update_params_t conn_params;
	memcpy(conn_params.bda, remote_bda, sizeof(esp_bd_addr_t));
	conn_params.latency = latency;
	conn_params.max_int = maxInterval;    // max_int = 0x20*1.25ms = 40ms
	conn_params.min_int = minInterval;    // min_int = 0x10*1.25ms = 20ms
	conn_params.timeout = timeout;    // timeout = 400*10ms = 4000ms
	esp_ble_gap_update_conn_params(&conn_params); 
}

BLEAddress BLEServer::getPeerAddress(){
	return m_client_addr;
}

int BLEServer::getRssi(){
	logger.debug(LOG_TAG, ">> getRssi()");

	if (m_connectedCount != 1) {
		logger.debug(LOG_TAG, String("<< getRssi(): error, ") + m_connectedCount + " devices connected");
		return 0;
	}
	
	// take the only device connected
	const std::map<uint16_t, conn_status_t>::iterator it = std::begin(m_connectedServersMap);


	// We make the API call to read the RSSI value which is an asynchronous operation.  We expect to receive
	// an ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT to indicate completion.
	//
	m_semaphoreRssiCmplEvt.take("getRssi");
	esp_err_t rc = ::esp_ble_gap_read_rssi(*(getPeerAddress().getNative()));
	//esp_err_t rc = ::esp_ble_gap_read_rssi(*(BLEAddress(std::string("80:5a:04:14:74:d7")).getNative()));
	if (rc != ESP_OK) {
		logger.error(LOG_TAG, String("<< getRssi: esp_ble_gap_read_rssi: rc=") + rc + " " + GeneralUtils::errorToString(rc));
		return 0;
	}
	int rssiValue = m_semaphoreRssiCmplEvt.wait("getRssi");
	logger.debug(LOG_TAG, String("<< getRssi(): ") + rssiValue);
	return rssiValue;
	}

	void BLEServer::handleGAPEvent(esp_gap_ble_cb_event_t  event, esp_ble_gap_cb_param_t* param){
		logger.debug(LOG_TAG, "handling GAP event!");
		switch (event) {
			//
			// ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT
			//
			// read_rssi_cmpl
			// - esp_bt_status_t status
			// - int8_t rssi
			// - esp_bd_addr_t remote_addr
			//
			case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT: {
				m_semaphoreRssiCmplEvt.give((uint32_t) param->read_rssi_cmpl.rssi);
				break;
			} // ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT

			default:
				break;
		}
}
#endif // CONFIG_BT_ENABLED
