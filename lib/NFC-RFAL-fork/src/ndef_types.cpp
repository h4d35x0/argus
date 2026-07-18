/******************************************************************************
  * \attention
  *
  * <h2><center>&copy; COPYRIGHT 2021 STMicroelectronics</center></h2>
  *
  * Licensed under ST MIX MYLIBERTY SOFTWARE LICENSE AGREEMENT (the "License");
  * You may not use this file except in compliance with the License.
  * You may obtain a copy of the License at:
  *
  *        www.st.com/mix_myliberty
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
  * AND SPECIFICALLY DISCLAIMING THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
******************************************************************************/

/*! \file
 *
 *  \author SRA
 *
 *  \brief NDEF RTD and MIME types
 *
 */

/*
 ******************************************************************************
 * INCLUDES
 ******************************************************************************
 */

#include "ndef_class.h"


/*
 ******************************************************************************
 * GLOBAL DEFINES
 ******************************************************************************
 */


/*
 ******************************************************************************
 * GLOBAL TYPES
 ******************************************************************************
 */


/*! NDEF type table to associate a ndefTypeId and a string */
typedef struct {
  ndefTypeId              typeId;        /*!< NDEF Type Id       */
  uint8_t                 tnf;           /*!< TNF                */
  const ndefConstBuffer8 *bufTypeString; /*!< Type String buffer */
} ndefTypeTable;


/*
 ******************************************************************************
 * LOCAL VARIABLES
 ******************************************************************************
 */


/*
 ******************************************************************************
 * LOCAL FUNCTION PROTOTYPES
 ******************************************************************************
 */


/*
 ******************************************************************************
 * GLOBAL FUNCTIONS
 ******************************************************************************
 */


/*****************************************************************************/
ReturnCode NdefClass::ndefTypeStringToTypeId(uint8_t tnf, const ndefConstBuffer8 *bufTypeString, ndefTypeId *typeId)
{
  /*! Empty string */
  static const uint8_t    ndefTypeEmpty[] = "";    /*!< Empty string */
  static ndefConstBuffer8 bufTypeEmpty    = { ndefTypeEmpty, sizeof(ndefTypeEmpty) - 1U };

  // TODO Transform the enum (u32) to defines (u8), re-order to u32-u8-u8 to compact buffer !
  static const ndefTypeTable typeTable[] = {
    { NDEF_TYPE_EMPTY,           NDEF_TNF_EMPTY,               &bufTypeEmpty              },
    { NDEF_TYPE_RTD_DEVICE_INFO, NDEF_TNF_RTD_WELL_KNOWN_TYPE, &bufRtdTypeDeviceInfo      },
    { NDEF_TYPE_RTD_TEXT,        NDEF_TNF_RTD_WELL_KNOWN_TYPE, &bufRtdTypeText            },
    { NDEF_TYPE_RTD_URI,         NDEF_TNF_RTD_WELL_KNOWN_TYPE, &bufRtdTypeUri             },
    { NDEF_TYPE_RTD_AAR,         NDEF_TNF_RTD_EXTERNAL_TYPE,   &bufRtdTypeAar             },
    { NDEF_TYPE_MEDIA_VCARD,     NDEF_TNF_MEDIA_TYPE,          &bufMediaTypeVCard         },
    { NDEF_TYPE_MEDIA_WIFI,      NDEF_TNF_MEDIA_TYPE,          &bufMediaTypeWifi          },
  };

  uint32_t i;

  if ((bufTypeString == NULL) || (typeId == NULL)) {
    return ST_ERR_PROTO;
  }

  for (i = 0; i < SIZEOF_ARRAY(typeTable); i++) {
    /* Check TNF and length are the same, then compare the content */
    if (typeTable[i].tnf == tnf) {
      if (bufTypeString->length == typeTable[i].bufTypeString->length) {
        if (bufTypeString->length == 0U) {
          /* Empty type */
          *typeId = typeTable[i].typeId;
          return ST_ERR_NONE;
        } else {
          if (ST_BYTECMP(typeTable[i].bufTypeString->buffer, bufTypeString->buffer, bufTypeString->length) == 0) {
            *typeId = typeTable[i].typeId;
            return ST_ERR_NONE;
          }
        }
      }
    }
  }

  return ST_ERR_NOTFOUND;
}


/*****************************************************************************/
ReturnCode NdefClass::ndefRecordTypeStringToTypeId(const ndefRecord *record, ndefTypeId *typeId)
{
  ReturnCode err;

  uint8_t          tnf;
  ndefConstBuffer8 bufRecordType;

  if ((record == NULL) || (typeId == NULL)) {
    return ST_ERR_PARAM;
  }

  err = ndefRecordGetType(record, &tnf, &bufRecordType);
  if (err != ST_ERR_NONE) {
    return err;
  }
  if (tnf >= NDEF_TNF_RESERVED) {
    return ST_ERR_INTERNAL;
  }

  switch (tnf) {
    case NDEF_TNF_EMPTY:               /* Fall through */
    case NDEF_TNF_RTD_WELL_KNOWN_TYPE: /* Fall through */
    case NDEF_TNF_RTD_EXTERNAL_TYPE:   /* Fall through */
    case NDEF_TNF_MEDIA_TYPE:          /* Fall through */
      err = ndefTypeStringToTypeId(tnf, &bufRecordType, typeId);
      break;
    default:
      err = ST_ERR_NOT_IMPLEMENTED;
      break;
  }

  return err;
}


/*****************************************************************************/
ReturnCode NdefClass::ndefRecordToType(const ndefRecord *record, ndefType *type)
{
  const ndefType *ndeftype;
  ReturnCode err;
  ndefTypeId typeId;

  ndeftype = ndefRecordGetNdefType(record);
  if (ndeftype != NULL) {
    /* Return the well-known type contained in the record */
    (void)ST_MEMCPY(type, ndeftype, sizeof(ndefType));
    return ST_ERR_NONE;
  }

  err = ndefRecordTypeStringToTypeId(record, &typeId);
  if (err != ST_ERR_NONE) {
    return err;
  }

  switch (typeId) {
    case NDEF_TYPE_EMPTY:
      return ndefRecordToEmptyType(record, type);
    case NDEF_TYPE_RTD_DEVICE_INFO:
      return ndefRecordToRtdDeviceInfo(record, type);
    case NDEF_TYPE_RTD_TEXT:
      return ndefRecordToRtdText(record, type);
    case NDEF_TYPE_RTD_URI:
      return ndefRecordToRtdUri(record, type);
    case NDEF_TYPE_RTD_AAR:
      return ndefRecordToRtdAar(record, type);
    case NDEF_TYPE_MEDIA_VCARD:
      return ndefRecordToVCard(record, type);
    case NDEF_TYPE_MEDIA_WIFI:
      return ndefRecordToWifi(record, type);
    default:
      return ST_ERR_NOT_IMPLEMENTED;
  }
}


/*****************************************************************************/
ReturnCode NdefClass::ndefTypeToRecord(const ndefType *type, ndefRecord *record)
{
  if (type == NULL) {
    return ST_ERR_PARAM;
  }

  switch (type->id) {
    case NDEF_TYPE_EMPTY:
      return ndefEmptyTypeToRecord(type, record);
    case NDEF_TYPE_RTD_DEVICE_INFO:
      return ndefRtdDeviceInfoToRecord(type, record);
    case NDEF_TYPE_RTD_TEXT:
      return ndefRtdTextToRecord(type, record);
    case NDEF_TYPE_RTD_URI:
      return ndefRtdUriToRecord(type, record);
    case NDEF_TYPE_RTD_AAR:
      return ndefRtdAarToRecord(type, record);
    case NDEF_TYPE_MEDIA_VCARD:
      return ndefVCardToRecord(type, record);
    case NDEF_TYPE_MEDIA_WIFI:
      return ndefWifiToRecord(type, record);
    default:
      return ST_ERR_NOT_IMPLEMENTED;
  }
}


/*****************************************************************************/
ReturnCode NdefClass::ndefRecordSetNdefType(ndefRecord *record, const ndefType *type)
{
  uint32_t payloadLength;

  if ((record == NULL) ||
      (type                   == NULL)               ||
      (type->id                > NDEF_TYPE_ID_COUNT) ||
      (type->getPayloadLength == NULL)               ||
      (type->getPayloadItem   == NULL)) {
    return ST_ERR_PARAM;
  }

  record->ndeftype = type;

  /* Set Short Record bit accordingly */
  payloadLength = ndefRecordGetPayloadLength(record);
  ndefHeaderSetValueSR(record, (payloadLength <= NDEF_SHORT_RECORD_LENGTH_MAX) ? 1 : 0);

  return ST_ERR_NONE;
}


/*****************************************************************************/
const ndefType *NdefClass::ndefRecordGetNdefType(const ndefRecord *record)
{
  if (record == NULL) {
    return NULL;
  }

  if (record->ndeftype != NULL) {
    /* Check whether it is a valid NDEF type */
    if ((record->ndeftype->id                < NDEF_TYPE_ID_COUNT) &&
        (record->ndeftype->getPayloadItem   != NULL) &&
        (record->ndeftype->getPayloadLength != NULL)) {
      return record->ndeftype;
    }
  }

  return NULL;
}


const char *NdefClass::errorToString(ReturnCode error_code)
{
    switch (error_code) {
      case ST_ERR_NONE:
          return "no error occurred";
      case ST_ERR_NOMEM:
          return "not enough memory to perform the requested operation";
      case ST_ERR_BUSY:
          return "device or resource busy";
      case ST_ERR_IO:
          return "generic IO error";
      case ST_ERR_TIMEOUT:
          return "error due to timeout";
      case ST_ERR_REQUEST:
          return "invalid request or requested function can't be executed at the moment";
      case ST_ERR_NOMSG:
          return "No message of desired type";
      case ST_ERR_PARAM:
          return "Parameter error";
      case ST_ERR_SYSTEM:
          return "System error";
      case ST_ERR_FRAMING:
          return "Framing error";
      case ST_ERR_OVERRUN:
          return "lost one or more received bytes";
      case ST_ERR_PROTO:
          return "protocol error";
      case ST_ERR_INTERNAL:
          return "Internal Error";
      case ST_ERR_AGAIN:
          return "Call again";
      case ST_ERR_MEM_CORRUPT:
          return "memory corruption";
      case ST_ERR_NOT_IMPLEMENTED:
          return "not implemented";
      case ST_ERR_PC_CORRUPT:
          return "Program Counter has been manipulated or spike/noise trigger illegal operation";
      case ST_ERR_SEND:
          return "error sending";
      case ST_ERR_IGNORE:
          return "indicates error detected but to be ignored";
      case ST_ERR_SEMANTIC:
          return "indicates error in state machine (unexpected cmd)";
      case ST_ERR_SYNTAX:
          return "indicates error in state machine (unknown cmd)";
      case ST_ERR_CRC:
          return "crc error";
      case ST_ERR_NOTFOUND:
          return "transponder not found";
      case ST_ERR_NOTUNIQUE:
          return "transponder not unique - more than one transponder in field";
      case ST_ERR_NOTSUPP:
          return "requested operation not supported";
      case ST_ERR_WRITE:
          return "write error";
      case ST_ERR_FIFO:
          return "fifo over or underflow error";
      case ST_ERR_PAR:
          return "parity error";
      case ST_ERR_DONE:
          return "transfer has already finished";
      case ST_ERR_RF_COLLISION:
          return "collision error (Bit Collision or during RF Collision avoidance )";
      case ST_ERR_HW_OVERRUN:
          return "lost one or more received bytes";
      case ST_ERR_RELEASE_REQ:
          return "device requested release";
      case ST_ERR_SLEEP_REQ:
          return "device requested sleep";
      case ST_ERR_WRONG_STATE:
          return "incorrect state for requested operation";
      case ST_ERR_MAX_RERUNS:
          return "blocking procedure reached maximum runs";
      case ST_ERR_DISABLED:
          return "operation aborted due to disabled configuration";
      case ST_ERR_HW_MISMATCH:
          return "expected hw do not match  ";
      case ST_ERR_LINK_LOSS:
          return "Other device's field didn't behave as expected: turned off by Initiator in Passive mode, or AP2P did not turn on field";
      case ST_ERR_INVALID_HANDLE:
          return "invalid or not initialized device handle";
      default:
          return "Unknown error code";
  }
}
