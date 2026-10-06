// src/modules/iot-attendance/dto/iot-scan.dto.ts
import { ApiProperty, ApiPropertyOptional } from '@nestjs/swagger';
import {
    IsString,
    IsNotEmpty,
    IsOptional,
    IsEnum,
    IsDateString,
    IsArray,
    ValidateNested,
    ArrayMaxSize,
    ArrayMinSize,
    MaxLength,
    IsBoolean,
    Matches,
} from 'class-validator';
import { Type } from 'class-transformer';

export enum IotScanMode {
    ENTRY = 'entry',
    EXIT = 'exit',
    REINFORCEMENT = 'reinforcement',
}

export class IotScanDto {
    @ApiProperty({ description: 'Student document number scanned from QR/barcode' })
    @IsString()
    @IsNotEmpty()
    @MaxLength(32)
    @Matches(/^\d{6,12}$/, { message: 'documentNumber must be 6-12 numeric digits' })
    documentNumber: string;

    @ApiProperty({ description: 'Scan timestamp from the device (ISO 8601)' })
    @IsDateString()
    scannedAt: string;

    @ApiPropertyOptional({ description: 'Idempotency key (UUIDv7 recommended)' })
    @IsOptional()
    @IsString()
    @MaxLength(64)
    scanId?: string;

    @ApiPropertyOptional({ enum: IotScanMode, default: IotScanMode.ENTRY })
    @IsOptional()
    @IsEnum(IotScanMode)
    mode?: IotScanMode = IotScanMode.ENTRY;

    @ApiPropertyOptional({ description: 'True if this scan was buffered while offline' })
    @IsOptional()
    @IsBoolean()
    offline?: boolean;

    // Extra fields the firmware may include for auditing/context.
    // Kept optional so `forbidNonWhitelisted` doesn't 400 the request.
    @ApiPropertyOptional({ description: 'Student schedule id (informational; backend uses student.classroom.academicScheduleId)' })
    @IsOptional()
    @IsString()
    @MaxLength(64)
    scheduleId?: string;

    @ApiPropertyOptional({ description: 'True when NTP was not synced at scan time (scannedAt may be inaccurate)' })
    @IsOptional()
    @IsBoolean()
    ntpUncertain?: boolean;

    @ApiPropertyOptional({ description: 'Local decision code from firmware classifier (0-9)' })
    @IsOptional()
    decision?: number;
}

export class IotBatchScanDto {
    @ApiProperty({ type: [IotScanDto], description: 'Scans to process (max 200)' })
    @IsArray()
    @ArrayMinSize(1)
    @ArrayMaxSize(200)
    @ValidateNested({ each: true })
    @Type(() => IotScanDto)
    scans: IotScanDto[];

    @ApiPropertyOptional({ description: 'Batch idempotency key for the whole batch' })
    @IsOptional()
    @IsString()
    @MaxLength(64)
    batchId?: string;
}

/**
 * Envelope used by POST /iot-attendance/scan/batch.
 *
 * Deliberately does NOT validate the individual scans: with `@ValidateNested`
 * a single malformed record invalidates the whole array and the global
 * ValidationPipe answers 400 for the entire request, before the service is
 * ever reached. A device draining its offline queue then retries the same
 * poisoned batch forever and never drains it — that is exactly how the
 * Scenario C run of 2026-09-26 lost all 6 buffered events (three of them
 * perfectly valid) to three records whose `scannedAt` was empty.
 *
 * Each record is validated on its own inside the service instead, so a bad
 * record comes back as an `invalid` outcome and the good ones still register.
 */
export class IotBatchScanEnvelopeDto {
    @ApiProperty({ type: [IotScanDto], description: 'Scans to process (max 200); validated per record' })
    @IsArray()
    @ArrayMinSize(1)
    @ArrayMaxSize(200)
    // `@Type(() => Object)` is load-bearing, not decoration. The global pipe runs
    // with `enableImplicitConversion: true`; without an element type it coerces
    // each record to the reflected type of the property itself (Array), handing
    // the service an Array object that still carries the record's properties.
    // Every record then fails the "is it an object?" check. Keep this.
    @Type(() => Object)
    scans: unknown[];

    @ApiPropertyOptional({ description: 'Batch idempotency key for the whole batch' })
    @IsOptional()
    @IsString()
    @MaxLength(64)
    batchId?: string;
}
