//
//  main.m
//  iogctl
//
//  Created by Jérémy Tran on 8/22/17.
//

// Include local headers before including the system headers, also include
// rather than import.
#include "IOGraphicsTypes.h"
#include "IOGraphicsTypesPrivate.h" // import DBG_IOG_* defines
#include "IOGraphicsDiagnose.h"     // import kIOGSharedInterface_* defines
#include "GMetricTypes.h"

#import <Foundation/Foundation.h>
#import <getopt.h>
#import <libgen.h>

#define COUNT_OF(x) \
    ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

#pragma mark - Command Constants
typedef NSString* IOGCommand NS_STRING_ENUM;
IOGCommand const IOGCommandInjectCS = @"injectCS";
IOGCommand const IOGCommandMetric = @"metric";

#pragma mark - IOGCommandInjectCS Command Constants
typedef NSString* IOGInjectCSCommand NS_STRING_ENUM;
IOGInjectCSCommand const IOGInjectCSCommandEnable = @"enable";
IOGInjectCSCommand const IOGInjectCSCommandDisable = @"disable";
IOGInjectCSCommand const IOGInjectCSCommandOpen = @"open";
IOGInjectCSCommand const IOGInjectCSCommandClose = @"close";

#pragma mark - IOGCommandMetric Command Constants
typedef NSString* IOGMetricCommand NS_STRING_ENUM;
IOGMetricCommand const IOGMetricCommandEnable = @"enable";
IOGMetricCommand const IOGMetricCommandDisable = @"disable";
IOGMetricCommand const IOGMetricCommandStart = @"start";
IOGMetricCommand const IOGMetricCommandStop = @"stop";
IOGMetricCommand const IOGMetricCommandReset = @"reset";
IOGMetricCommand const IOGMetricCommandFetch = @"fetch";

#pragma mark - GMetric Formats
typedef enum : NSInteger {
    GMetricFormatDefault,
    GMetricFormatJSON,
} GMetricFormat;

#pragma mark - GMetric Domain Name Constants
typedef NSString* GMetricDomainName NS_STRING_ENUM;
GMetricDomainName const GMetricDomainNameSleep = @"sleep";
GMetricDomainName const GMetricDomainNameWake = @"wake";
GMetricDomainName const GMetricDomainNameDoze = @"doze";
GMetricDomainName const GMetricDomainNameDarkWake = @"darkwake";
GMetricDomainName const GMetricDomainNameFramebuffer = @"framebuffer";
GMetricDomainName const GMetricDomainNamePower = @"power";
GMetricDomainName const GMetricDomainNameDisplayWrangler = @"displaywrangler";
GMetricDomainName const GMetricDomainNameAll = @"all";

#pragma mark - Globals
NSDictionary<NSNumber *, NSString *> *gTypeMap;
NSDictionary<NSNumber *, NSString *> *gFuncMap;
NSDictionary<NSNumber *, NSString *> *gMarkerMap;
NSDictionary<NSString *, NSNumber *> *gDomainsMap;
NSDictionary<NSString *, NSNumber *> *gInjectCSCmdMap;
NSDictionary<NSString *, NSNumber *> *gMetricCmdMap;

void initGlobals() {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        gTypeMap = @{@(kGMETRICS_EVENT_START): @"start",
                     @(kGMETRICS_EVENT_END): @"end",
                     @(kGMETRICS_EVENT_SIGNAL): @"signal",
                     };
        gFuncMap = @{@(DBG_IOG_NOTIFY_SERVER): @"notifyServer",
                     @(DBG_IOG_SERVER_ACK): @"serverAck",
                     @(DBG_IOG_VRAM_RESTORE): @"vramRestore",
                     @(DBG_IOG_VRAM_BLACK): @"vramBlack",
                     @(DBG_IOG_WSAA_DEFER_ENTER): @"wsaaDeferEnter",
                     @(DBG_IOG_WSAA_DEFER_EXIT): @"wsaaDeferExit",
                     @(DBG_IOG_SET_POWER_STATE): @"setPowerState",
                     @(DBG_IOG_SYSTEM_POWER_CHANGE): @"systemPowerChange",
                     @(DBG_IOG_ACK_POWER_STATE): @"ackPowerState",
                     @(DBG_IOG_SET_POWER_ATTRIBUTE): @"setPowerAttribute",
                     @(DBG_IOG_ALLOW_POWER_CHANGE): @"allowPowerChange",
                     @(DBG_IOG_MUX_ALLOW_POWER_CHANGE): @"muxAllowPowerChange",
                     @(DBG_IOG_SERVER_TIMEOUT): @"serverTimeout",
                     @(DBG_IOG_NOTIFY_CALLOUT): @"notifyCallout",
                     @(DBG_IOG_MUX_POWER_MESSAGE): @"muxPowerMessage",
                     @(DBG_IOG_FB_POWER_CHANGE): @"fbPowerChange",
                     @(DBG_IOG_WAKE_FROM_DOZE): @"wakeFromDoze",
                     @(DBG_IOG_RECEIVE_POWER_NOTIFICATION): @"receivePowerNotification",
                     @(DBG_IOG_CHANGE_POWER_STATE_PRIV): @"changePowerStateToPriv",
                     @(DBG_IOG_CLAMP_POWER_ON): @"clampPowerOn",
                     @(DBG_IOG_SET_TIMER_PERIOD): @"setTimerPeriod",
                     @(DBG_IOG_HANDLE_EVENT): @"handleEvent",
                     @(DBG_IOG_SET_ATTRIBUTE_EXT): @"setAttributeExt",
                     @(DBG_IOG_CLAMSHELL): @"clamshell",
                     @(DBG_IOG_HANDLE_VBL_INTERRUPT): @"handleVBLInterrupt",
                     @(DBG_IOG_WAIT_QUIET): @"waitQuiet",
                     @(DBG_IOG_PLATFORM_CONSOLE): @"platformConsole",
                     @(DBG_IOG_CONSOLE_CONFIG): @"consoleConfig",
                     @(DBG_IOG_VRAM_CONFIG): @"vramConfig",
                     @(DBG_IOG_SET_GAMMA_TABLE): @"setGammaTable",
                     @(DBG_IOG_NEW_USER_CLIENT): @"newUserClient",
                     @(DBG_IOG_FB_CLOSE): @"fbClose",
                     @(DBG_IOG_SET_DISPLAY_MODE): @"setDisplayMode",
                     @(DBG_IOG_SET_DETAILED_TIMING): @"setDetailedTiming",
                     };
        gMarkerMap = @{@(kGMETRICS_MARKER_SLEEP): @"Sleep",
                       @(kGMETRICS_MARKER_ENTERING_SLEEP): @"Entering sleep",
                       @(kGMETRICS_MARKER_EXITING_SLEEP): @"Exiting sleep",
                       @(kGMETRICS_MARKER_DOZE): @"Doze",
                       @(kGMETRICS_MARKER_DARKWAKE): @"DarkWake",
                       @(kGMETRICS_MARKER_WAKE): @"Wake",
                       @(kGMETRICS_MARKER_ENTERING_WAKE): @"Entering wake",
                       @(kGMETRICS_MARKER_EXITING_WAKE): @"Exiting wake",
                       @(kGMETRICS_MARKER_BOOT): @"Boot",
                       };
        gDomainsMap = @{GMetricDomainNameSleep: @(kGMETRICS_DOMAIN_SLEEP),
                        GMetricDomainNameWake: @(kGMETRICS_DOMAIN_WAKE),
                        GMetricDomainNameDoze: @(kGMETRICS_DOMAIN_DOZE),
                        GMetricDomainNameDarkWake: @(kGMETRICS_DOMAIN_DARKWAKE),
                        GMetricDomainNamePower: @(kGMETRICS_DOMAIN_POWER),
                        GMetricDomainNameFramebuffer: @(kGMETRICS_DOMAIN_FRAMEBUFFER),
                        GMetricDomainNameDisplayWrangler: @(kGMETRICS_DOMAIN_DISPLAYWRANGLER),
                        GMetricDomainNameAll: @(kGMETRICS_DOMAIN_ALL)
                        };
        gInjectCSCmdMap = @{IOGInjectCSCommandEnable: [NSNumber numberWithUnsignedLongLong:'Enbl'],
                            IOGInjectCSCommandDisable: [NSNumber numberWithUnsignedLongLong:'Dsbl'],
                            IOGInjectCSCommandOpen: [NSNumber numberWithUnsignedLongLong:'Open'],
                            IOGInjectCSCommandClose: [NSNumber numberWithUnsignedLongLong:'Clos'],
                            };
        gMetricCmdMap = @{IOGMetricCommandEnable: @(kGMetricCmdEnable),
                          IOGMetricCommandDisable: @(kGMetricCmdDisable),
                          IOGMetricCommandStart: @(kGMetricCmdStart),
                          IOGMetricCommandStop: @(kGMetricCmdStop),
                          IOGMetricCommandReset: @(kGMetricCmdReset),
                          IOGMetricCommandFetch: @(kGMetricCmdFetch),
                          };
    });
}

#pragma mark -

void printUsage(const char *cmdName, char *helpSection) {
    NSString *help = helpSection != NULL ? [NSString stringWithUTF8String:helpSection] : nil;

    if ([help isEqualToString:IOGCommandInjectCS]) {
        printf("usage: %s injectCS.<cmd>\n\n"
               "<cmd> can take one of the following values:\n\n"
               "enable\n\tEnable clamshell state injection.\n"
               "disable\n\tDisable clamshell state injection.\n"
               "open\n\tSend an open clamshell event.\n"
               "close\n\tSend a close clamshell event.\n", cmdName);
    } else if ([help isEqualToString:IOGCommandMetric]) {
        printf("usage: %s metric.<cmd> [--count|-c <number>] [--domain|-d <domains>] [--format|-f <format>]\n\n"
               "<cmd> can take one of the following values:\n\n"

               "enable\n\tAllocate a memory buffer to record metrics. If the --count option is provided, it limits the max number of metrics to record (default: %i, max: %i).\n"

               "disable\n\tDeallocate metrics memory.\n"

               "start\n\tStart recording metrics. The --domain option needs to be provided and its argument should be a comma-separated list of metric domains to record.\n"
                   "\tValid domains are: wake, sleep, doze, darkwake, power, framebuffer, displaywrangler, and all.\n"

               "stop\n\tStop recording metrics.\n"

               "reset\n\tDiscard existing recorded metrics, without deallocating the metrics buffer memory.\n"

               "fetch\n\tPrint recorded metrics. If the --count option is provided, it limits the number of metrics to return. "
                   "The optional --format option specifies the format to output the metrics (valid value: json).\n",
               cmdName, kGMetricDefaultLineCount, kGMetricMaximumLineCount);
    } else {
        printf("usage: %s [-h] cmd.subcmd [options]\n\n"
               "Supported commands:\n"
               "\tinjectCS.[enable|disable|open|close]\n\n"
               "\tmetric.[enable|disable|start|stop|reset|fetch]\n\n"
               "For more info, use the --help=<argument> option with 'metric' or 'injectCS' as the argument.\n",
               cmdName);
    }
}

IOReturn invokeInjectCS(io_connect_t connect, uint64_t injectCSCmd)
{
    uint64_t scalarParams[] = { injectCSCmd };
    uint32_t scalarParamsCount = COUNT_OF(scalarParams);
    IOReturn err = IOConnectCallMethod(connect, kIOGDUCInterface_clamshell,
                                       scalarParams, scalarParamsCount,
                                       NULL, 0, NULL, NULL, NULL, NULL);

    return err;
}

NSArray<NSDictionary *> *arrayFromMetricsBuffer(const gmetric_buffer_t *buffer,
                                                NSInteger entriesCount) {
    NSMutableArray<NSDictionary *> *metrics = [@[] mutableCopy];

    for (NSInteger i = 0; i < entriesCount; ++i) {
        const gmetric_entry_t * const entry = &buffer->fEntries[i];
        const uint16_t func   = GMETRIC_FUNC_FROM_DATA(entry->data);
        const uint16_t marker = GMETRIC_MARKER_FROM_DATA(entry->data);

        if (func) {
            NSMutableDictionary *entryDict = [@{
                    @"type": gTypeMap[@(entry->header.type)],
                    @"func": gFuncMap[@(func)],
                    @"cpu": @(entry->header.cpu),
                    @"tid": @(entry->tid),
                    @"domain": @(entry->header.domain),
                    @"timestamp": @(entry->timestamp),
                } mutableCopy];
            if (marker)
                entryDict[@"marker"] = gMarkerMap[@(marker)];
            [metrics addObject: entryDict];
        } else {
            NSLog(@"gmetric_buffer.entry[%d] does not have a func provided,"
                   " ignoring", (int) i);
        }

    }

    return metrics;
}

IOReturn invokeMetric(io_connect_t connect,
                      uint64_t metricCmd,
                      uint64_t startParam,
                      uint64_t enableParam,
                      uint64_t fetchCount,
                      GMetricFormat format) {
    uint64_t scalarParams[] = { metricCmd, startParam, enableParam };
    uint32_t scalarParamsCount = COUNT_OF(scalarParams);
    NSMutableData *fetchData = nil;
    gmetric_buffer_t *fetchBuffer = NULL;

    if (!fetchCount || fetchCount > kGMetricMaximumLineCount)
        fetchCount = kGMetricMaximumLineCount;
    NSUInteger fetchBufferSize
        = sizeof(gmetric_buffer_t) + fetchCount * sizeof(gmetric_entry_t);
    if (kGMetricCmdFetch == metricCmd) {
        fetchData = [NSMutableData dataWithLength: fetchBufferSize];
        fetchBuffer = fetchData.mutableBytes;
    }

    IOReturn err = IOConnectCallMethod(
            connect, kIOGDUCInterface_metrics,
            scalarParams, scalarParamsCount, NULL, 0,   // Inputs
            NULL, NULL, fetchBuffer, &fetchBufferSize); // Outputs

    if ((kIOReturnSuccess == err) && (metricCmd == kGMetricCmdFetch)) {
        const NSInteger bufferCount = fetchBuffer->fHeader.fEntriesCount;
        const NSInteger entriesCount = MIN(fetchCount, bufferCount);
        NSArray<NSDictionary *> *metrics
            = arrayFromMetricsBuffer(fetchBuffer, entriesCount);

        switch (format) {
        case GMetricFormatDefault:
            printf("Entries count: %u\n", (int) bufferCount);
            for (NSInteger i = 0; i < metrics.count; ++i) {
                NSDictionary<NSString *, id> *metric = metrics[i];
                printf("[%lu]", i);
                if (metric[@"marker"]) {
                    printf(" ====== %s ======", [metric[@"marker"] UTF8String]);
                }
                printf(" type: %s, func: %s, cpu: %lu, thread: 0x%llx, domain: 0x%llx, timestamp: %llu\n",
                       [metric[@"type"] UTF8String],
                       [metric[@"func"] UTF8String],
                       [metric[@"cpu"] unsignedIntegerValue],
                       [metric[@"tid"] unsignedLongLongValue],
                       [metric[@"domain"] unsignedLongLongValue],
                       [metric[@"timestamp"] unsignedLongLongValue]);
            }
            break;

        case GMetricFormatJSON:
            printf("%s\n", [[NSJSONSerialization dataWithJSONObject:metrics options:0 error:nil] bytes]);
            break;
        }
    }

    return err;
}

IOReturn sendCmd(IOReturn (^invokeCommand)(io_connect_t))
{
    static const char * const
        sWranglerPath = kIOServicePlane ":/IOResources/IODisplayWrangler";
    io_registry_entry_t wrangler = IO_OBJECT_NULL;

    wrangler = IORegistryEntryFromPath(kIOMasterPortDefault, sWranglerPath);
    if (IO_OBJECT_NULL == wrangler) {
        fprintf(stderr, "IODisplayWrangler search failed");
        return kIOReturnNotFound;
    }

    io_connect_t wranglerConnect = IO_OBJECT_NULL;
    IOReturn err = IOServiceOpen(wrangler, mach_task_self(),
                                 kIOGDiagnoseConnectType, &wranglerConnect);
    if (kIOReturnSuccess == err) {
        err = invokeCommand(wranglerConnect);
        IOServiceClose(wranglerConnect);
        if (kIOReturnSuccess != err)
            fprintf(stderr, "User client method call failed.\n");
    } else
        fprintf(stderr, "IOService open failed.\n");
    IOObjectRelease(wrangler);

    return err;
}

int main(int argc, char * const argv[]) {
    kern_return_t err;
    char cmdBuffer[MAXPATHLEN];
    const char * const cmdName = cmdBuffer;
    basename_r(argv[0], cmdBuffer);

    @autoreleasepool {
        static struct option longopts[] = {
            { "help", optional_argument, NULL, 'h' },
            { "count", required_argument, NULL, 'c' },
            { "domain", required_argument, NULL, 'd' },
            { "format", required_argument, NULL, 'f' },
            { NULL, 0, NULL, 0 }
        };
        int ch;
        NSString *cmd = nil;
        NSArray<NSString *> *cmdList = nil;
        IOReturn (^invokeCommand)(io_connect_t) = nil;
        uint64_t count = UINT64_MAX;
        uint64_t domain = kGMETRICS_DOMAIN_NONE;
        GMetricFormat format = GMetricFormatDefault;

        initGlobals();

        while ((ch = getopt_long(argc, argv, "h?c:d:f:", longopts, NULL)) != -1) {
            switch (ch) {
                case 'h': {
                    printUsage(cmdName, optarg);
                    return 0;
                }

                case 'c': {
                    count = [[NSString stringWithUTF8String:optarg] longLongValue];
                    break;
                }

                case 'd': {
                    NSString *domainsArg = [NSString stringWithUTF8String:optarg];
                    for (NSString *domainString in [domainsArg componentsSeparatedByString:@","]) {
                        domain |= [gDomainsMap[domainString] unsignedLongLongValue];
                    }
                    break;
                }

                case 'f': {
                    NSString *optStr = [NSString stringWithUTF8String:optarg];
                    if ([optStr isEqualToString:@"json"]) {
                        format = GMetricFormatJSON;
                    }
                    break;
                }

                default: {
                    err = kIOReturnBadArgument;
                    goto error;
                }
            }
        }
        argc -= optind;
        argv += optind;

        // There should be a single remaining argument for the command to execute
        if (argc != 1) {
            err = kIOReturnError;
            goto error;
        }

        // Remaining argument is a string of the form: command.subcommand
        cmd = [NSString stringWithUTF8String:argv[0]];
        cmdList = [cmd componentsSeparatedByString:@"."];

        if ([cmdList[0] isEqualToString:IOGCommandInjectCS]) {
            NSNumber *injectCSCmd = cmdList.count > 1 ? gInjectCSCmdMap[cmdList[1]] : nil;
            if (nil == injectCSCmd) {
                err = kIOReturnBadArgument;
                goto error;
            }
            invokeCommand = ^IOReturn(io_connect_t connect) {
                return invokeInjectCS(connect, [injectCSCmd unsignedLongLongValue]);
            };
        } else if ([cmdList[0] isEqualToString:IOGCommandMetric]) {
            NSNumber *metricCmd = cmdList.count > 1 ? gMetricCmdMap[cmdList[1]] : nil;
            if (nil == metricCmd) {
                err = kIOReturnBadArgument;
                goto error;
            }
            // Depending on the command to execute, the -c flag has a slightly
            // different use.
            // If `count` == UINT64_MAX it means the user didn't provide the -c
            // flag. Therefore, use the default value.
            const uint64_t metricsCount =
                (metricCmd.unsignedLongLongValue == kGMetricCmdEnable && count != UINT64_MAX) ? count : 0;
            // No need to test for UINT64_MAX since IOG won't fetch more metrics
            // than it has in store.
            const uint64_t fetchCount = metricCmd.unsignedLongLongValue == kGMetricCmdFetch ? count : 0;

            invokeCommand = ^IOReturn(io_connect_t connect) {
                return invokeMetric(connect,
                                    metricCmd.unsignedLongLongValue,
                                    domain,
                                    metricsCount,
                                    fetchCount,
                                    format);
            };
        } else {
            err = kIOReturnBadArgument;
            goto error;
        }

        err = sendCmd(invokeCommand);
    }
    return err;

error:
    printUsage(cmdName, NULL);
    return err;
}
