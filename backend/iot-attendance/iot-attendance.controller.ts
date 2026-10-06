// src/modules/iot-attendance/iot-attendance.controller.ts
import {
    Body,
    Controller,
    HttpCode,
    HttpStatus,
    Post,
    Request,
    UseGuards,
} from '@nestjs/common';
import { ApiOperation, ApiTags } from '@nestjs/swagger';
import { Throttle } from '@nestjs/throttler';
import { DeviceAuthGuard } from '../../common/guards/device-auth.guard';
import { Public } from '../../common/decorators/public.decorator';
import { CurrentDevice } from '../../common/decorators/current-device.decorator';
import type { IotDevice } from '@prisma/client';
import { IotAttendanceService } from './iot-attendance.service';
import { IotScanDto, IotBatchScanEnvelopeDto } from './dto/iot-scan.dto';

@ApiTags('IoT Attendance')
@Public() // Skip JWT — DeviceAuthGuard handles auth
@UseGuards(DeviceAuthGuard)
@Controller('iot-attendance')
export class IotAttendanceController {
    constructor(private readonly service: IotAttendanceService) { }

    @Post('scan')
    @HttpCode(HttpStatus.OK)
    @Throttle({ default: { limit: 30, ttl: 60_000 } })
    @ApiOperation({ summary: 'Register a single attendance scan from an IoT device' })
    async scan(@Body() dto: IotScanDto, @CurrentDevice() device: IotDevice) {
        return this.service.processScan(device, dto);
    }

    @Post('scan/batch')
    @HttpCode(HttpStatus.OK)
    @Throttle({ default: { limit: 6, ttl: 60_000 } })
    @ApiOperation({
        summary: 'Register a batch of attendance scans (offline sync)',
        description:
            'Records are validated individually: a malformed record comes back with status "invalid" ' +
            'and the rest still register. The request only fails as a whole if the envelope itself is ' +
            'malformed, so a device draining its offline queue can never be blocked by one bad record.',
    })
    async batch(@Body() dto: IotBatchScanEnvelopeDto, @CurrentDevice() device: IotDevice) {
        return this.service.processBatch(device, dto);
    }
}
