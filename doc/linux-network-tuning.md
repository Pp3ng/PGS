# Linux Network Performance Tuning

This document outlines key system parameters for optimizing network performance on Linux systems.

## Overview

The Linux kernel provides various system parameters that can be tuned to optimize network performance. These parameters can be adjusted using the `sysctl` command to improve connection handling, reduce latency, and enhance overall system performance.

## Key Parameters

### Socket Backlog Configuration

```bash
sysctl -w net.core.somaxconn=65535
```

- **Parameter**: `net.core.somaxconn`
- **Default Value**: Usually 128 or 1024
- **Recommended Value**: 65535
- **Description**: Defines the maximum length of the listen queue for accepting new TCP connections. Increasing this value allows handling more concurrent connection requests.
- **Use Case**: High-traffic web servers, load balancers

### TCP SYN Backlog

```bash
sysctl -w net.ipv4.tcp_max_syn_backlog=65535
```

- **Parameter**: `net.ipv4.tcp_max_syn_backlog`
- **Default Value**: Usually 1024
- **Recommended Value**: 65535
- **Description**: Maximum number of remembered connection requests which have not received an acknowledgment from the connecting client
- **Impact**: Helps prevent SYN flood attacks and handles high-volume connection requests

### Network Device Backlog

```bash
sysctl -w net.core.netdev_max_backlog=65535
```

- **Parameter**: `net.core.netdev_max_backlog`
- **Default Value**: Usually 1000
- **Recommended Value**: 65535
- **Description**: Maximum number of packets queued on the input side of network device
- **Impact**: Prevents packet drops during high network load

### TCP FIN Timeout

```bash
sysctl -w net.ipv4.tcp_fin_timeout=30
```

- **Parameter**: `net.ipv4.tcp_fin_timeout`
- **Default Value**: 60 seconds
- **Recommended Value**: 30 seconds
- **Description**: Time to hold socket in FIN-WAIT-2 state before forcing closure
- **Impact**: Faster release of system resources for closed connections

### TCP Keepalive Time

```bash
sysctl -w net.ipv4.tcp_keepalive_time=300
```

- **Parameter**: `net.ipv4.tcp_keepalive_time`
- **Default Value**: 7200 seconds (2 hours)
- **Recommended Value**: 300 seconds (5 minutes)
- **Description**: Time interval between keepalive probes
- **Impact**: Faster detection of dead connections

### TIME_WAIT Buckets

```bash
sysctl -w net.ipv4.tcp_max_tw_buckets=262144
```

- **Parameter**: `net.ipv4.tcp_max_tw_buckets`
- **Default Value**: Usually 16384
- **Recommended Value**: 262144
- **Description**: Maximum number of sockets in TIME_WAIT state
- **Impact**: Prevents resource exhaustion under high connection churn

## Making Changes Permanent

To make these changes permanent across system reboots, add the following lines to `/etc/sysctl.conf`:

```bash
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_fin_timeout = 30
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_max_tw_buckets = 262144
```

Then apply the changes:

```bash
sysctl -p
```

## Monitoring

To verify the current values of these parameters, use:

```bash
sysctl -a | grep 'net.core.somaxconn\|net.ipv4.tcp_max_syn_backlog\|net.core.netdev_max_backlog\|net.ipv4.tcp_fin_timeout\|net.ipv4.tcp_keepalive_time\|net.ipv4.tcp_max_tw_buckets'
```

## Best Practices

1. Always test changes in a non-production environment first
2. Monitor system performance before and after changes
3. Adjust values based on your specific workload and hardware capabilities
4. Document any changes made to system parameters
5. Keep track of performance metrics to validate improvements

## Security Considerations

While these settings can improve performance, they should be balanced against security requirements:

- Higher backlog values might increase vulnerability to DDoS attacks
- Shorter timeouts may impact legitimate long-running connections
- Consider implementing additional security measures like rate limiting

## Additional Resources

- Linux kernel documentation: [/proc/sys/net documentation](https://www.kernel.org/doc/Documentation/networking/ip-sysctl.txt)
- TCP/IP stack tuning guide
- System monitoring tools documentation
