// src/modules/iot-attendance/iot-attendance.module.ts
import { Module } from '@nestjs/common';
import { PrismaModule } from '../../prisma/prisma.module';
import { IotAttendanceController } from './iot-attendance.controller';
import { IotAttendanceService } from './iot-attendance.service';

@Module({
    imports: [PrismaModule],
    controllers: [IotAttendanceController],
    providers: [IotAttendanceService],
    exports: [IotAttendanceService],
})
export class IotAttendanceModule { }
