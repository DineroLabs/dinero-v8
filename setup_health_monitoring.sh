#!/bin/bash
# Set up health check cron job on Mac
# Monitors all Dinero nodes for version drift and sync issues

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
HEALTH_CHECK="$SCRIPT_DIR/health_check.sh"
LOG_FILE="$HOME/dinero-health.log"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🏥 Setting up Dinero Health Check Monitoring"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check if health_check.sh exists
if [[ ! -f "$HEALTH_CHECK" ]]; then
    echo "❌ Error: health_check.sh not found at $HEALTH_CHECK"
    exit 1
fi

# Make sure it's executable
chmod +x "$HEALTH_CHECK"

# Check if cron job already exists
CRON_LINE="*/30 * * * * $HEALTH_CHECK >> $LOG_FILE 2>&1"
if crontab -l 2>/dev/null | grep -q "$HEALTH_CHECK"; then
    echo "⚠️  Health check cron job already exists:"
    crontab -l | grep "$HEALTH_CHECK"
    echo ""
    read -p "Do you want to replace it? (y/N) " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "❌ Aborted."
        exit 0
    fi
    # Remove existing cron job
    crontab -l | grep -v "$HEALTH_CHECK" | crontab -
fi

# Add new cron job
(crontab -l 2>/dev/null; echo "$CRON_LINE") | crontab -

echo "✅ Health check cron job installed!"
echo ""
echo "Schedule: Every 30 minutes"
echo "Script:   $HEALTH_CHECK"
echo "Log file: $LOG_FILE"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📋 Configuration (optional)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "To enable email alerts, edit health_check.sh and set:"
echo "  ALERT_EMAIL=\"your@email.com\""
echo ""
echo "To enable Slack alerts, edit health_check.sh and set:"
echo "  SLACK_WEBHOOK=\"https://hooks.slack.com/services/YOUR/WEBHOOK/URL\""
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🧪 Testing health check now..."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Run health check immediately
"$HEALTH_CHECK"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ Setup complete!"
echo ""
echo "Commands:"
echo "  View logs:      tail -f $LOG_FILE"
echo "  Manual check:   $HEALTH_CHECK"
echo "  List cron jobs: crontab -l"
echo "  Remove cron:    crontab -l | grep -v health_check | crontab -"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

