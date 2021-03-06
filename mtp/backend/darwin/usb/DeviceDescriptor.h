/*
    This file is part of Android File Transfer For Linux.
    Copyright (C) 2015-2016  Vladimir Menshakov

    Android File Transfer For Linux is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    Android File Transfer For Linux is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Android File Transfer For Linux.
    If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <mtp/types.h>
#include <usb/Device.h>
#include <usb/Interface.h>

#include <usb/usb.h>

#include <vector>

namespace mtp { namespace usb
{
	class Configuration : Noncopyable
	{
		//IOUSBDeviceType			**_dev;
		IOUSBConfigurationDescriptorPtr	_conf;

		std::vector<IOUSBInterfaceInterface **> _interfaces;

	public:
		Configuration(IOUSBDeviceType ** dev, IOUSBConfigurationDescriptorPtr conf);
		~Configuration() { }

		int GetIndex() const
		{ return _conf->bConfigurationValue; }

		int GetInterfaceCount() const
		{ return _interfaces.size(); }

		int GetInterfaceAltSettingsCount(int idx) const
		{ return 1; }

		InterfacePtr GetInterface(DevicePtr device, ConfigurationPtr config, int idx, int settings) const
		{ return std::make_shared<Interface>(device, config, _interfaces.at(idx)); }
	};

	class DeviceDescriptor
	{
	private:
		IOUSBDeviceType			**_dev;

	public:
		DeviceDescriptor(io_service_t desc);
		~DeviceDescriptor();

		u16 GetVendorId() const;
		u16 GetProductId() const;

		DevicePtr Open(ContextPtr context);
		DevicePtr TryOpen(ContextPtr context);

		int GetConfigurationsCount() const;

		ConfigurationPtr GetConfiguration(int conf);
		ByteArray GetDescriptor();
	};
	DECLARE_PTR(DeviceDescriptor);

}}

#endif

